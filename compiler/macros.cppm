module;
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module lyra.macros;
import lyra.check;
import lyra.platform;

export namespace lyra {
using MacroFiles = std::map<std::string, std::map<std::string, std::filesystem::path>>;
struct MacroPackage { std::filesystem::path root; MacroFiles inputs; };
void expandMacros(Module& syntax, std::vector<Source>& sources, const std::map<std::string, MacroPackage>& packages, std::vector<std::filesystem::path>& inputs);
void parsePackageFile(Module& syntax, Source& source, std::size_t file, const MacroPackage& package, std::vector<std::filesystem::path>& inputs);
}

namespace lyra {
namespace fs = std::filesystem;
namespace {
using Value = std::variant<std::monostate, bool, std::int64_t, std::string>;
struct Returned { Value value; };
constexpr std::size_t byteLimit = 1024 * 1024;
constexpr auto prelude = R"(fn fieldCount(): Int { 0 }
fn fieldName(index: Int): String { "" }
fn fieldType(index: Int): String { "" }
fn fieldPublic(index: Int): Bool { false }
fn fieldHasAttribute(index: Int, name: String): Bool { false }
fn fieldAttributeValue(index: Int, name: String): String { "" }
fn argumentCount(): Int { 0 }
fn argumentCode(index: Int): String { "" }
fn argumentString(index: Int): String { "" }
fn targetKind(): String { "" }
fn targetBody(): String { "" }
fn optionInner(typeName: String): String { "" }
fn listElement(typeName: String): String { "" }
fn macroQuote(text: String): String { "" }
fn macroInput(key: String): String { "" }
fn macroFail(message: String): String { "" }
)";

std::string quote(const std::string& text) {
    std::string result = "\"";
    for (char ch : text) {
        switch (ch) {
            case '"': case '\\': case '$': result += '\\'; result += ch; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            case '\0': result += "\\0"; break;
            default: result += ch;
        }
    }
    return result + '"';
}

struct Machine {
    const CheckedModule& program;
    const Source& use;
    const Expansion& expansion;
    const std::vector<Field>& fields;
    const std::string& kind;
    const std::string& body;
    const MacroPackage& package;
    std::vector<fs::path>& files;
    const ExpressionExpansion* expression;
    unsigned fuel = 50000, depth = 0;
    std::size_t bytes = 0;
    std::map<std::string, std::string> inputs{};

    [[noreturn]] void error(const std::string& message) const { throw at(use, expansion.name.span, "LYM0002", message); }
    void budget(std::size_t size) {
        bytes += size;
        if (size > byteLimit || bytes > 32 * byteLimit) error("宏展开超出字节预算");
    }
    Value input(const std::string& key) {
        if (auto saved = inputs.find(key); saved != inputs.end()) return saved->second;
        auto grants = package.inputs.find(expansion.name.text);
        if (grants == package.inputs.end() || !grants->second.contains(key)) error("宏未获得只读输入：" + key);
        auto path = package.root / grants->second.at(key);
        auto relative = fs::weakly_canonical(path).lexically_relative(fs::canonical(package.root));
        if (relative.empty() || *relative.begin() == ".." || !fs::is_regular_file(path) || fs::file_size(path) > byteLimit) error("只读宏输入不存在、越界或过大：" + key);
        files.push_back(path);
        return inputs.emplace(key, loadSource(path, {}).text).first->second;
    }

    Value host(const std::string& name, const std::vector<Value>& args) {
        if (name == "fieldCount") return static_cast<std::int64_t>(fields.size());
        if (name == "targetKind") return kind;
        if (name == "targetBody") return body;
        if (name == "optionInner" || name == "listElement") {
            auto& type = std::get<std::string>(args.front());
            std::string prefix = name == "optionInner" ? "Option<" : "List<";
            return type.starts_with(prefix) && type.ends_with('>') ? type.substr(prefix.size(), type.size() - prefix.size() - 1) : std::string{};
        }
        if (name == "macroFail") error(std::get<std::string>(args.front()));
        if (name == "macroQuote") return quote(std::get<std::string>(args.front()));
        if (name == "macroInput") return input(std::get<std::string>(args.front()));
        if (name == "argumentCount" || name == "argumentCode" || name == "argumentString") {
            if (!expression) error("此查询仅适用于表达式宏");
            if (name == "argumentCount") return static_cast<std::int64_t>(expression->arguments.size());
            auto index = std::get<std::int64_t>(args.front());
            if (index < 0 || static_cast<std::size_t>(index) >= expression->arguments.size()) error("表达式宏实参下标越界");
            if (name == "argumentCode") return "__lyraMacroArg" + std::to_string(index);
            const auto& arg = expression->arguments[index];
            if (!arg.literal) throw at(use, arg.span, "LYM0002", "此宏实参需要字符串字面量");
            return *arg.literal;
        }
        auto index = std::get<std::int64_t>(args.front());
        if (index < 0 || static_cast<std::size_t>(index) >= fields.size()) error("宏读取的字段或形参下标越界");
        const auto& field = fields[index];
        if (name == "fieldName") return field.name.text;
        if (name == "fieldType") return field.type.text;
        if (name == "fieldHasAttribute" || name == "fieldAttributeValue") {
            auto found = std::ranges::find_if(field.attributes, [&](const auto& mark) { return mark.name.text == std::get<std::string>(args[1]); });
            if (name == "fieldHasAttribute") return found != field.attributes.end();
            if (found == field.attributes.end()) error("字段或形参缺少标记：" + std::get<std::string>(args[1]));
            return found->argument ? found->argument->text : std::string{};
        }
        return field.isPublic;
    }

    Value invoke(const CheckedFunction& fn, const std::vector<Value>& args) {
        if (fn.syntax.file == 0) return host(fn.syntax.name.text, args);
        if (++depth > 64) error("宏调用深度超过 64");
        std::vector<Value> locals(fn.locals.size());
        std::copy(args.begin(), args.end(), locals.begin());
        Value result;
        try { result = eval(fn, fn.syntax.body, locals); } catch (Returned& ret) { result = std::move(ret.value); }
        --depth;
        return result;
    }

    Value math(Op op, const Value& left, const Value& right) {
        if (op == Op::Eq || op == Op::Ne) return op == Op::Eq ? left == right : left != right;
        if (op == Op::Add && std::holds_alternative<std::string>(left)) return std::get<std::string>(left) + std::get<std::string>(right);
        auto first = std::get<std::int64_t>(left), second = std::get<std::int64_t>(right);
        std::int64_t result;
        switch (op) {
            case Op::Add: if (!__builtin_add_overflow(first, second, &result)) return result; break;
            case Op::Sub: if (!__builtin_sub_overflow(first, second, &result)) return result; break;
            case Op::Mul: if (!__builtin_mul_overflow(first, second, &result)) return result; break;
            case Op::Div: if (second && !(first == std::numeric_limits<std::int64_t>::min() && second == -1)) return first / second; break;
            case Op::Rem: if (second == -1) return std::int64_t{0}; if (second) return first % second; break;
            case Op::Lt: return first < second;
            case Op::Le: return first <= second;
            case Op::Gt: return first > second;
            case Op::Ge: return first >= second;
            default: error("此运算不能用于编译期宏");
        }
        error("宏中整数溢出或除零");
    }

    Value eval(const CheckedFunction& fn, ExprId id, std::vector<Value>& locals) {
        if (!fuel--) error("宏展开超过执行步数预算");
        auto sub = [&](ExprId child) { return eval(fn, child, locals); };
        const auto& info = fn.expressions[id];
        const auto& node = fn.syntax.expressions[id].node;
        Value result;
        if (auto value = std::get_if<std::int64_t>(&node)) result = *value;
        else if (auto value = std::get_if<bool>(&node)) result = *value;
        else if (auto str = std::get_if<StringLiteral>(&node)) result = str->bytes;
        else if (std::holds_alternative<Identifier>(node)) result = locals[info.target];
        else if (auto binding = std::get_if<Binding>(&node)) locals[info.target] = sub(binding->value);
        else if (auto block = std::get_if<Block>(&node)) {
            for (auto& statement : block->statements) { result = sub(statement.expression); if (statement.semicolon) result = {}; }
        } else if (auto branch = std::get_if<Conditional>(&node)) {
            if (std::get<bool>(sub(branch->condition))) result = sub(branch->yes);
            else if (branch->no) result = sub(*branch->no);
        } else if (auto loop = std::get_if<While>(&node)) {
            while (std::get<bool>(sub(loop->condition))) {
                try { sub(loop->body); } catch (Jump jump) { if (jump == Jump::Break) break; }
            }
        } else if (auto jump = std::get_if<Jump>(&node)) throw *jump;
        else if (auto ret = std::get_if<Return>(&node)) throw Returned{ret->value ? sub(*ret->value) : Value{}};
        else if (auto unary = std::get_if<Unary>(&node)) {
            auto operand = sub(unary->operand);
            result = unary->op == Op::Not ? Value(!std::get<bool>(operand)) : math(Op::Sub, std::int64_t{0}, operand);
        } else if (auto binary = std::get_if<Binary>(&node)) {
            auto op = binary->op;
            if (op <= Op::RemSet) {
                if (!std::holds_alternative<Identifier>(fn.syntax.expressions[binary->left].node)) error("宏中只能写入局部绑定");
                auto slot = fn.expressions[binary->left].target;
                auto left = locals[slot];
                auto right = sub(binary->right);
                constexpr Op operations[] = {Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Rem};
                locals[slot] = op == Op::Set ? right : math(operations[static_cast<int>(op) - 1], left, right);
                if (auto str = std::get_if<std::string>(&locals[slot])) budget(str->size());
            } else {
                auto left = sub(binary->left);
                if (op == Op::And) result = std::get<bool>(left) && std::get<bool>(sub(binary->right));
                else if (op == Op::Or) result = std::get<bool>(left) || std::get<bool>(sub(binary->right));
                else result = math(op, left, sub(binary->right));
            }
        } else if (auto call = std::get_if<Call>(&node)) {
            if (info.callee != Callee::Function || call->receiver) error("编译期不能调用运行时函数：" + call->name.text);
            const auto& callee = program.functions[info.target];
            if (call->arguments.size() != callee.syntax.parameters.size()) error("编译期宏调用需要显式传入每个形参");
            std::vector<Value> args(call->arguments.size());
            for (std::size_t index = 0; index < args.size(); ++index) args[info.argumentSlots[index]] = sub(call->arguments[index].value);
            result = invoke(callee, args);
        } else error("此表达式不属于受限的编译期 Lyra 子集");
        if (auto str = std::get_if<std::string>(&result)) budget(str->size());
        return result;
    }
};

std::string expand(const Source& source, const Expansion& prefix, const MacroPackage& package, const std::string& target, const std::vector<Field>& fields, const std::string& kind, const std::string& body, std::vector<fs::path>& files, const ExpressionExpansion* expression = nullptr) {
    auto directory = expression ? "expressions" : prefix.decorator ? "decorators" : "macros";
    auto file = prefix.name.text + ".ly";
    auto path = package.root / "src" / (std::string(".") + directory) / file;
    if (fs::is_regular_file(path)) {
        auto relative = fs::weakly_canonical(path).lexically_relative(fs::canonical(package.root));
        if (relative.empty() || *relative.begin() == "..") throw at(source, prefix.name.span, "LYM0002", "宏实现必须位于声明包内");
    } else path = sdkRoot() / "share/lyra/std/meta" / directory / file;
    if (!fs::is_regular_file(path)) throw at(source, prefix.name.span, "LYM0001", "找不到" + std::string(expression ? "表达式宏 #" : prefix.decorator ? "装饰器 @" : "声明宏 #") + prefix.name.text);
    if (fs::file_size(path) > byteLimit) throw at(source, prefix.name.span, "LYM0002", "宏实现超过大小限制");
    files.push_back(path);
    auto builtin = sourceText("<macro-prelude>", prelude, {});
    std::vector<Source> code{std::move(builtin), loadSource(path, {})};
    auto parsed = parse(code);
    if (!parsed.imports.empty() || !parsed.aggregates.empty() || !parsed.expansions.empty() || !parsed.traits.empty()) throw at(source, prefix.name.span, "LYM0002", "宏实现只能声明同步的纯计算函数");
    for (const auto& fn : parsed.functions) if (fn.isAsync || fn.foreignSymbol || !fn.typeParameters.empty()) throw at(source, prefix.name.span, "LYM0002", "宏不能使用 extern、异步或泛型入口");
    auto checked = check(std::move(parsed), code, false);
    auto scalar = [](Type type) { return type == Type::Int || type == Type::Bool || type == Type::String || type == Type::Unit || type == Type::Never; };
    for (const auto& fn : checked.functions) {
        if (!scalar(fn.result) || !std::ranges::all_of(fn.locals, [&](const auto& local) { return scalar(local.type); }) ||
            !std::ranges::all_of(fn.expressions, [&](const auto& expr) { return scalar(expr.type); })) {
            throw at(source, prefix.name.span, "LYM0002", "编译期宏只可计算 Int、Bool、String 和 Unit");
        }
    }
    auto it = std::find_if(checked.functions.begin(), checked.functions.end(), [&](const auto& fn) { return fn.syntax.file == 1 && fn.syntax.name.text == prefix.name.text; });
    if (it == checked.functions.end() || it->result != Type::String || it->syntax.parameters.size() != 2 || it->locals[0].type != Type::String || it->locals[1].type != Type::String) {
        throw at(source, prefix.name.span, "LYM0002", "宏入口应为 fn " + prefix.name.text + "(target: String, argument: String): String");
    }
    Machine machine{checked, source, prefix, fields, kind, body, package, files, expression};
    return std::get<std::string>(machine.invoke(*it, {target, prefix.argument ? prefix.argument->text : std::string{}}));
}
}

void parsePackageFile(Module& syntax, Source& source, std::size_t file, const MacroPackage& package, std::vector<fs::path>& inputs) {
    const auto limit = source.text.size() + 4 * byteLimit;
    for (unsigned depth = 0; source.text.find('#') != std::string::npos; ++depth) {
        Module scratch;
        std::vector<ExpressionExpansion> calls;
        parseFile(scratch, source, file, &calls);
        if (calls.empty()) break;
        if (depth == 32) throw at(source, calls.front().name.span, "LYM0002", "表达式宏嵌套超过 32 层");
        std::string text;
        std::vector<SourcePiece> pieces;
        std::size_t read = 0;
        const auto original = std::string_view(source.text);
        for (const auto& call : calls) {
            auto site = call.name.span;
            auto append = [&](std::string_view bytes, Span input, bool copied, std::size_t hygiene) {
                if (bytes.empty()) return;
                if (bytes.size() > limit - text.size()) throw at(source, site, "LYM0002", "展开后源码超过字节预算");
                pieces.push_back({{text.size(), text.size() + bytes.size()}, input, copied, hygiene});
                text += bytes;
            };
            auto copy = [&](Span span) { append(original.substr(span.begin, span.end - span.begin), span, true, 0); };
            copy({read, call.span.begin});
            auto argument = call.argumentsSpan;
            Expansion prefix{file, call.name, Identifier{std::string(original.substr(argument.begin, argument.end - argument.begin)), argument}, false, false, 0};
            auto output = sourceText(fromUtf8(pathText(source.path) + "(#" + call.name.text + ")"), expand(source, prefix, package, "", {}, "expression", "", inputs, &call), source.moduleId);
            output.origin = source.site(site);
            auto splices = macroSplices(output, call.arguments.size());
            auto mark = output.origin->start.offset + 1;
            append("(", site, false, mark);
            std::size_t cursor = 0;
            auto generated = std::string_view(output.text);
            for (auto splice : splices) {
                append(generated.substr(cursor, splice.span.begin - cursor), site, false, mark);
                auto input = call.arguments[splice.argument].span;
                append("(", input, false, 0); copy(input); append(")", input, false, 0);
                cursor = splice.span.end;
            }
            append(generated.substr(cursor), site, false, mark);
            append("\n)", site, false, mark);
            read = call.span.end;
        }
        if (limit - text.size() < original.size() - read) throw at(source, calls.back().name.span, "LYM0002", "展开后源码超过字节预算");
        if (read < original.size()) {
            pieces.push_back({{text.size(), text.size() + original.size() - read}, {read, original.size()}, true, 0});
            text += original.substr(read);
        }
        auto next = sourceText(source.path, std::move(text), source.moduleId);
        next.previous = std::make_shared<Source>(std::move(source));
        next.pieces = std::move(pieces);
        source = std::move(next);
    }
    parseFile(syntax, source, file);
}

void expandMacros(Module& syntax, std::vector<Source>& sources, const std::map<std::string, MacroPackage>& packages, std::vector<fs::path>& inputs) {
    auto prefixes = std::exchange(syntax.expansions, {});
    std::map<std::size_t, std::size_t> hooks;
    for (const auto& prefix : prefixes) {
        const auto& source = sources[prefix.file];
        if (prefix.decorator && prefix.name.text == "derive") throw at(source, prefix.name.span, "LYM0001", "derive 是声明宏；请写 #derive(Json)");
        if (prefix.decorator && prefix.record) throw at(source, prefix.name.span, "LYM0001", "函数装饰器不能标在类型上");
        auto& name = prefix.record ? syntax.aggregates[prefix.target].name : syntax.functions[prefix.target].name;
        std::vector<Field> fields;
        std::string bodyText;
        if (prefix.record) fields = syntax.aggregates[prefix.target].fields;
        else {
            auto& fn = syntax.functions[prefix.target];
            for (const auto& param : fn.parameters) fields.push_back({param.name, param.type, false, true, param.attributes});
            if (!fn.foreignSymbol) { auto span = fn.expressions[fn.body].span; bodyText = source.text.substr(span.begin, span.end - span.begin); }
        }
        auto generated = expand(source, prefix, packages.at(source.moduleId), name.text, fields, prefix.record ? (syntax.aggregates[prefix.target].reference ? "class" : "struct") : "fn", bodyText, inputs);
        auto label = prefix.decorator ? "@" : "#";
        if (prefix.decorator) generated = "fn hook(): Unit {\n" + generated + "\n}";
        auto output = sourceText(fromUtf8(pathText(source.path) + "(" + label + prefix.name.text + ")"), std::move(generated), source.moduleId);
        output.origin = source.site(prefix.name.span);
        sources.push_back(std::move(output));
        auto start = syntax.functions.size(), records = syntax.aggregates.size(), imports = syntax.imports.size(), traits = syntax.traits.size();
        parseFile(syntax, sources.back(), sources.size() - 1);
        if (!syntax.expansions.empty() || std::any_of(syntax.traits.begin() + traits, syntax.traits.end(), [](const auto& trait) { return trait.isPublic; }) || std::any_of(syntax.functions.begin() + start, syntax.functions.end(), [](const auto& fn) { return fn.isPublic || fn.foreignSymbol; }) ||
            std::any_of(syntax.aggregates.begin() + records, syntax.aggregates.end(), [](const auto& record) { return record.isPublic; }) ||
            std::any_of(syntax.imports.begin() + imports, syntax.imports.end(), [](const auto& use) { return use.isPublic; })) {
            throw at(sources[prefix.file], prefix.name.span, "LYM0002", "宏不能自行生成公开声明、extern 或新的宏调用");
        }
        if (prefix.decorator) {
            auto& fn = syntax.functions[prefix.target];
            if (fn.foreignSymbol || syntax.functions.size() != start + 1 || syntax.aggregates.size() != records || syntax.imports.size() != imports || syntax.traits.size() != traits) throw at(sources[prefix.file], prefix.name.span, "LYM0002", "入口装饰器需要普通函数体");
            auto hook = "$decorator" + std::to_string(start);
            syntax.functions[start].name.text = hook;
            auto id = fn.expressions.size();
            fn.expressions.push_back({prefix.name.span, Call{{hook, prefix.name.span}, {}}});
            auto& body = std::get<Block>(fn.expressions[fn.body].node).statements;
            body.insert(body.begin() + hooks[prefix.target]++, {id, prefix.name.span});
        }
    }
}
}
