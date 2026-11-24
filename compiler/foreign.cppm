module;
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

export module lyra.foreign;
import lyra.check;
import lyra.numbers;

export namespace lyra {
void validateForeign(const CheckedFunction& function, const Source& source);
void validateForeignDeclarations(const CheckedModule& program, const std::vector<Source>& sources);
void emitForeign(const CheckedFunction& function, const Source& source, llvm::Function& wrapper);
}

namespace lyra {
namespace {

bool abiType(Type type) {
    return numericType(type) || type == Type::Bool || type == Type::String || type == Type::Bytes || type == Type::CPtr;
}

llvm::Type* cType(Type type, llvm::IRBuilder<>& builder) {
    switch (type.kind) {
        case Type::Int: case Type::UInt: return builder.getIntNTy(numberWidth(type));
        case Type::Real: return numberWidth(type) == 32 ? builder.getFloatTy() : builder.getDoubleTy();
        case Type::Bool: return builder.getInt8Ty();
        case Type::Unit: return builder.getVoidTy();
        case Type::CPtr: case Type::String: case Type::Bytes: return builder.getPtrTy();
        default: llvm_unreachable("unsupported foreign ABI type");
    }
}

llvm::Attribute::AttrKind extension(Type type) {
    if (type == Type::Bool || (type.kind == Type::UInt && numberWidth(type) < 32)) return llvm::Attribute::ZExt;
    if (type.kind == Type::Int && numberWidth(type) < 32) return llvm::Attribute::SExt;
    return llvm::Attribute::None;
}

}

void validateForeign(const CheckedFunction& fn, const Source& source) {
    const auto& symbol = *fn.syntax.foreignSymbol;
    if (symbol.text == "main" || symbol.text == "wmain" || symbol.text.starts_with("llvm.")) throw at(source, symbol.span, "LYF0001", "不能绑定此保留 C 符号：" + symbol.text);
    for (std::size_t slot = 0; slot < fn.syntax.parameters.size(); ++slot) {
        if (!abiType(fn.locals[slot].type)) throw at(source, fn.syntax.parameters[slot].type.span, "LYF0001", "extern 形参只支持数值、Bool、String 或 Bytes 句柄，以及 CPtr");
    }
    if (fn.result != Type::Unit && !abiType(fn.result)) throw at(source, fn.syntax.result.span, "LYF0001", "此返回类型没有可用的 C ABI");
}

void validateForeignDeclarations(const CheckedModule& program, const std::vector<Source>& sources) {
    std::map<std::string, std::vector<Type>> signatures;
    for (const auto& fn : program.functions) {
        if (!fn.syntax.foreignSymbol) continue;
        validateForeign(fn, sources[fn.syntax.file]);
        std::vector<Type> signature{fn.result};
        for (std::size_t slot = 0; slot < fn.syntax.parameters.size(); ++slot) signature.push_back(fn.locals[slot].type);
        const auto& symbol = *fn.syntax.foreignSymbol;
        auto [entry, inserted] = signatures.emplace(symbol.text, signature);
        if (!inserted && entry->second != signature) throw at(sources[fn.syntax.file], symbol.span, "LYF0001", "此 C 符号有不一致的 extern 签名：" + symbol.text);
    }
}

void emitForeign(const CheckedFunction& fn, const Source& source, llvm::Function& wrapper) {
    auto& module = *wrapper.getParent();
    llvm::IRBuilder<> builder(module.getContext());
    builder.SetInsertPoint(llvm::BasicBlock::Create(module.getContext(), "ffi", &wrapper));
    std::vector<llvm::Type*> parameters;
    std::vector<llvm::Value*> values;
    std::vector<llvm::AllocaInst*> borrowedRoots;
    for (auto& argument : wrapper.args()) {
        auto type = fn.locals[argument.getArgNo()].type;
        auto raw = cType(type, builder);
        parameters.push_back(raw);
        values.push_back(type == Type::Bool ? builder.CreateZExt(&argument, raw) : &argument);
        if (type == Type::String || type == Type::Bytes) {
            auto root = builder.CreateAlloca(raw, nullptr, "borrowed.buffer");
            builder.CreateStore(&argument, root, true);
            borrowedRoots.push_back(root);
        }
    }
    const auto& symbol = *fn.syntax.foreignSymbol;
    auto signature = llvm::FunctionType::get(cType(fn.result, builder), parameters, false);
    if (auto previous = module.getFunction(symbol.text); previous && previous->getFunctionType() != signature) throw at(source, symbol.span, "LYF0001", "此符号的 C ABI 签名冲突：" + symbol.text);
    auto foreign = llvm::cast<llvm::Function>(module.getOrInsertFunction(symbol.text, signature).getCallee());
    auto result = builder.CreateCall(foreign, values);
    for (unsigned slot = 0; slot < parameters.size(); ++slot) if (auto attr = extension(fn.locals[slot].type); attr != llvm::Attribute::None) {
        foreign->addParamAttr(slot, attr);
        result->addParamAttr(slot, attr);
    }
    if (auto attr = extension(fn.result); attr != llvm::Attribute::None) { foreign->addRetAttr(attr); result->addRetAttr(attr); }
    for (auto root : borrowedRoots) builder.CreateLoad(builder.getPtrTy(), root, true);
    if (fn.result == Type::Unit) builder.CreateRet(builder.getInt8(0));
    else if (fn.result == Type::Bool) builder.CreateRet(builder.CreateIsNotNull(result));
    else builder.CreateRet(result);
}

}
