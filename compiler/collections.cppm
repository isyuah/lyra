module;
#include <cstddef>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module lyra.collections;
import lyra.source;

export namespace lyra {

enum class ArgumentMode { Fixed, Item, Spread };
struct ParameterShape { std::string_view name; bool defaulted, variadic; };
struct ArgumentShape { std::optional<std::string_view> label; bool spread; Span site; bool trailing{}; };
struct ArgumentSlot { std::size_t index; ArgumentMode mode; };
struct CallPlan {
    std::vector<ArgumentSlot> explicitSlots;
    std::vector<std::size_t> defaults;
    std::optional<std::size_t> variadic;
};

CallPlan bindArguments(std::span<const ParameterShape> parameters, std::span<const ArgumentShape> arguments,
                       const Source& source, Span call, std::string_view name) {
    CallPlan plan;
    if (!parameters.empty() && parameters.back().variadic) plan.variadic = parameters.size() - 1;
    auto fixed = plan.variadic.value_or(parameters.size());
    std::vector<std::optional<Span>> filled(fixed);
    std::size_t position = 0;
    bool named = false;
    for (const auto& arg : arguments) {
        if (arg.label || arg.trailing) {
            named = true;
            std::size_t slot = arg.trailing ? parameters.size() - 1 : 0;
            if (arg.label) {
                while (slot < parameters.size() && parameters[slot].name != *arg.label) ++slot;
                if (slot == parameters.size()) throw at(source, arg.site, "LYT0002", "未知的命名实参：" + std::string(*arg.label));
                if (slot == plan.variadic) throw at(source, arg.site, "LYT0002", "变长形参只接收位置实参：" + std::string(*arg.label));
            }
            if (filled[slot]) {
                auto error = at(source, arg.site, "LYT0002", "重复绑定形参：" + (parameters[slot].name.empty() ? std::string("末尾函数形参") : std::string(parameters[slot].name)));
                auto previous = source.origin ? *source.origin : source.site(*filled[slot]);
                error.hint = "先前的实参位于 " + previous.file + ':' + std::to_string(previous.start.line) + ':' + std::to_string(previous.start.column);
                throw error;
            }
            if (arg.spread) throw at(source, arg.site, "LYT0002", "展开只能填入变长形参");
            filled[slot] = arg.site;
            plan.explicitSlots.push_back({slot, ArgumentMode::Fixed});
        } else {
            if (named) throw at(source, arg.site, "LYT0002", "位置实参须在命名实参之前");
            if (arg.spread) {
                if (!plan.variadic) throw at(source, arg.site, "LYT0002", "没有可接收展开元素的变长形参");
                position = fixed;
            }
            if (position < fixed) {
                filled[position] = arg.site;
                plan.explicitSlots.push_back({position++, ArgumentMode::Fixed});
            } else if (plan.variadic) {
                plan.explicitSlots.push_back({*plan.variadic, arg.spread ? ArgumentMode::Spread : ArgumentMode::Item});
            } else throw at(source, arg.site, "LYT0002", std::string(name) + " 的位置实参过多");
        }
    }
    for (std::size_t slot = 0; slot < fixed; ++slot) {
        if (filled[slot]) continue;
        if (!parameters[slot].defaulted) throw at(source, call, "LYT0002", std::string(name) + " 缺少形参：" + std::string(parameters[slot].name));
        plan.defaults.push_back(slot);
    }
    return plan;
}

struct NativeLists {
    llvm::IRBuilder<>& builder;

    llvm::Module& module() const { return *builder.GetInsertBlock()->getModule(); }

    llvm::Value* create() const {
        return builder.CreateCall(module().getOrInsertFunction("lyraListCreate", builder.getPtrTy()));
    }

    llvm::Value* size(llvm::Value* list) const {
        return builder.CreateCall(module().getOrInsertFunction("lyraListSize", builder.getInt64Ty(), builder.getPtrTy()), {list});
    }

    llvm::Value* box(llvm::Value* value) const {
        auto allocate = module().getOrInsertFunction("lyraGcAlloc", builder.getPtrTy(), builder.getInt64Ty());
        auto snapshot = builder.CreateCall(allocate, {llvm::ConstantExpr::getSizeOf(value->getType())});
        builder.CreateStore(value, snapshot);
        return snapshot;
    }

    void add(llvm::Value* list, llvm::Value* value) const {
        auto pointer = builder.getPtrTy();
        builder.CreateCall(module().getOrInsertFunction("lyraListAdd", builder.getVoidTy(), pointer, pointer), {list, box(value)});
    }

    void extend(llvm::Value* list, llvm::Value* values) const {
        auto pointer = builder.getPtrTy();
        builder.CreateCall(module().getOrInsertFunction("lyraListExtend", builder.getVoidTy(), pointer, pointer), {list, values});
    }

    llvm::Value* get(llvm::Value* list, llvm::Value* index, const Source& source, Span site) const {
        auto pointer = builder.getPtrTy(); auto integer = builder.getInt64Ty();
        auto position = source.position(site.begin);
        auto access = module().getOrInsertFunction("lyraListGet", pointer, pointer, integer, pointer, integer, integer);
        return builder.CreateCall(access, {list, index, builder.CreateGlobalStringPtr(source.path.string()), builder.getInt64(position.line), builder.getInt64(position.column)});
    }

    void set(llvm::Value* list, llvm::Value* index, llvm::Value* value) const {
        auto pointer = builder.getPtrTy();
        builder.CreateCall(module().getOrInsertFunction("lyraListSet", builder.getVoidTy(), pointer, builder.getInt64Ty(), pointer), {list, index, box(value)});
    }
};

}
