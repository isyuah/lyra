module;
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

export module lyra.conversions;
import lyra.platform;
import lyra.check;

export namespace lyra {
struct DirectConversion { Type source, target; std::size_t function; };

std::string conversionSite(const CheckedFunction& declaration, const std::vector<Source>& sources) {
    const auto& file = sources[declaration.syntax.file];
    return pathText(file.path) + ":" + std::to_string(file.position(declaration.syntax.name.span.begin).line);
}

void registerConversion(std::vector<DirectConversion>& table, const CheckedModule& module, const std::vector<Source>& sources, std::size_t index) {
    const auto& fn = module.functions[index];
    const auto& site = sources[fn.syntax.file];
    auto source = fn.locals.front().type, target = fn.result;
    auto owner = [&](Type type) {
        if (type.kind == Type::Record) return sources[module.aggregates[type.id].syntax.file].moduleId == site.moduleId;
        if (type.kind == Type::Enum) return sources[module.enums[type.id].syntax.file].moduleId == site.moduleId;
        if (type.kind == Type::Exception && type.id >= firstDeclaredException) return sources[module.exceptions[type.id - firstDeclaredException].file].moduleId == site.moduleId;
        return false;
    };
    if (source == target || (!owner(source) && !owner(target))) throw at(site, fn.syntax.name.span, "LYV0001", "implicit 的源或目标类型必须由本模块定义，且不能是同一类型");
    if (fn.syntax.isPublic && (hiddenType(module, source) || hiddenType(module, target))) throw at(site, fn.syntax.name.span, "LYV0001", "pub implicit 的两端类型必须公开");
    for (const auto& conversion : table) {
        const auto& existing = module.functions[conversion.function];
        if (conversion.source == source && conversion.target == target && sources[existing.syntax.file].moduleId == site.moduleId) {
            throw at(site, fn.syntax.name.span, "LYV0001", "此模块已声明相同方向的 implicit：" + conversionSite(existing, sources));
        }
    }
    table.push_back({source, target, index});
}

std::optional<std::size_t> directConversion(const std::vector<DirectConversion>& table, const CheckedModule& module,
        const std::vector<Source>& sources, Type from, Type to, const Source& use, Span site, bool requireUnique = true) {
    std::optional<std::size_t> result;
    for (const auto& conversion : table) {
        if (conversion.source != from || conversion.target != to) continue;
        const auto& fn = module.functions[conversion.function];
        if (!fn.syntax.isPublic && sources[fn.syntax.file].moduleId != use.moduleId) continue;
        if (!requireUnique) return conversion.function;
        if (result) throw at(use, site, "LYV0002", fn.syntax.parameters.front().type.text + " → " + fn.syntax.result.text + " 存在两条可见的直接 implicit 转换：" + conversionSite(module.functions[*result], sources) + "、" + conversionSite(fn, sources));
        result = conversion.function;
    }
    return result;
}
}
