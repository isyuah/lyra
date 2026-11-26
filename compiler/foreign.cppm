module;
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <algorithm>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module lyra.foreign;
import lyra.check;
import lyra.closures;
import lyra.exceptions;
import lyra.numbers;

export namespace lyra {
bool callbackAbiType(Type type);
void validateForeignDeclarations(const CheckedModule& program, const std::vector<Source>& sources);
void emitForeign(const CheckedFunction& function, const Source& source, llvm::Function& wrapper);
llvm::Value* emitCallback(llvm::IRBuilder<>& builder, Type type, const FunctionShape& shape,
                         llvm::FunctionType* closureSignature, llvm::Value* closure, llvm::Value* fallback);
}

namespace lyra {
bool callbackAbiType(Type type) { return numericType(type) || type == Type::Bool || type == Type::CPtr; }

namespace {

bool abiType(Type type) {
    return callbackAbiType(type) || type == Type::String || type == Type::Bytes;
}

llvm::Type* cType(Type type, llvm::IRBuilder<>& builder) {
    switch (type.kind) {
        case Type::Int: case Type::UInt: return builder.getIntNTy(numberWidth(type));
        case Type::Real: return numberWidth(type) == 32 ? builder.getFloatTy() : builder.getDoubleTy();
        case Type::Bool: return builder.getInt8Ty();
        case Type::Unit: return builder.getVoidTy();
        case Type::CPtr: case Type::String: case Type::Bytes: case Type::Task: return builder.getPtrTy();
        default: llvm_unreachable("unsupported foreign ABI type");
    }
}

llvm::Attribute::AttrKind extension(Type type) {
    if (type == Type::Bool || (type.kind == Type::UInt && numberWidth(type) < 32)) return llvm::Attribute::ZExt;
    if (type.kind == Type::Int && numberWidth(type) < 32) return llvm::Attribute::SExt;
    return llvm::Attribute::None;
}

void validateForeign(const CheckedFunction& fn, const CheckedModule& program, const Source& source) {
    const auto& symbol = *fn.syntax.foreignSymbol;
    struct SdkBinding { std::string_view module, symbol; std::vector<Type> parameters; Type result; bool task; };
    static const std::vector<SdkBinding> bindings{
        {"$std/http", "lyraHttpAcceptAsync", {Type::CPtr, Type::Int, Type::Bool, Type::Bool}, Type::CPtr, true},
        {"$std/http", "lyraHttpReadBodyAsync", {Type::CPtr}, Type::CPtr, true},
        {"$std/http", "lyraHttpRespondAsync", {Type::CPtr, Type::Int, Type::String, Type::Bytes, Type::CPtr}, Type::Int, true},
        {"$std/http", "lyraHttpStartStreamAsync", {Type::CPtr, Type::Int, Type::String, Type::CPtr}, Type::Int, true},
        {"$std/http", "lyraHttpWriteStreamAsync", {Type::CPtr, Type::Bytes, Type::Bool}, Type::Int, true},
        {"$std/http", "lyraHttpWaitDisconnectAsync", {Type::CPtr, Type::Int}, Type::Int, true},
        {"$std/http", "lyraHttpRequestCloseAsync", {Type::CPtr}, Type::Unit, true},
        {"$std/http", "lyraHttpListenerCloseAsync", {Type::CPtr}, Type::Unit, true},
        {"$std/http", "lyraHttpListenerScopeAsync", {Type::CPtr}, Type::Unit, true},
        {"$std/http", "lyraHttpKeepListener", {Type::CPtr}, Type::Unit, false},
        {"$std/httpClient", "lyraHttpClientSendAsync", {Type::CPtr}, Type::CPtr, true},
        {"$std/httpClient", "lyraHttpClientOpenAsync", {Type::CPtr}, Type::CPtr, true},
        {"$std/httpClient", "lyraHttpClientReadAsync", {Type::CPtr}, Type::CPtr, true},
        {"$std/httpClient", "lyraHttpClientCloseAsync", {Type::CPtr}, Type::CPtr, true},
        {"$std/os", "lyraOsWaitSignal", {Type::Int}, Type::Int, true},
    };
    if (auto binding = std::ranges::find(bindings, symbol.text, &SdkBinding::symbol); binding != bindings.end()) {
        bool result = binding->task ? fn.result.kind == Type::Task && program.taskResults[fn.result.id] == binding->result
                                    : fn.result == binding->result;
        if (source.moduleId != binding->module || !result || fn.syntax.parameters.size() != binding->parameters.size())
            throw at(source, symbol.span, "LYF0001", "此符号仅供内置运行时使用：" + symbol.text);
        for (std::size_t slot = 0; slot < binding->parameters.size(); ++slot)
            if (fn.locals[slot].type != binding->parameters[slot])
                throw at(source, fn.syntax.parameters[slot].type.span, "LYF0001", "内置运行时签名不匹配");
        return;
    }
    if (symbol.text == "main" || symbol.text == "wmain" || symbol.text.starts_with("llvm.")) throw at(source, symbol.span, "LYF0001", "不能绑定此保留 C 符号：" + symbol.text);
    for (std::size_t slot = 0; slot < fn.syntax.parameters.size(); ++slot) {
        if (!abiType(fn.locals[slot].type)) throw at(source, fn.syntax.parameters[slot].type.span, "LYF0001", "extern 形参只支持数值、Bool、String 或 Bytes 句柄，以及 CPtr");
    }
    if (fn.result != Type::Unit && !abiType(fn.result)) throw at(source, fn.syntax.result.span, "LYF0001", "此返回类型没有可用的 C ABI");
}

}

void validateForeignDeclarations(const CheckedModule& program, const std::vector<Source>& sources) {
    std::map<std::string, std::vector<Type>> signatures;
    for (const auto& fn : program.functions) {
        if (!fn.syntax.foreignSymbol) continue;
        validateForeign(fn, program, sources[fn.syntax.file]);
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

llvm::Value* emitCallback(llvm::IRBuilder<>& builder, Type type, const FunctionShape& shape,
                         llvm::FunctionType* closureSignature, llvm::Value* closure, llvm::Value* fallback) {
    auto& module = *builder.GetInsertBlock()->getModule();
    auto& context = module.getContext();
    auto* pointer = builder.getPtrTy();
    auto name = "lyra.callback." + std::to_string(type.id);
    auto* trampoline = module.getFunction(name);
    if (!trampoline) {
        std::vector<llvm::Type*> params{pointer};
        for (auto param : shape.parameters) params.push_back(cType(param, builder));
        auto* signature = llvm::FunctionType::get(cType(shape.result, builder), params, false);
        trampoline = llvm::Function::Create(signature, llvm::Function::InternalLinkage, name, module);
        auto* entry = llvm::BasicBlock::Create(context, "entry", trampoline);
        auto* failed = llvm::BasicBlock::Create(context, "failed", trampoline);
        auto* finished = llvm::BasicBlock::Create(context, "finished", trampoline);
        llvm::IRBuilder<> emit(entry);
        auto* callback = trampoline->getArg(0);
        auto registerThread = module.getOrInsertFunction("lyraGcEnterWorker", emit.getInt32Ty());
        auto unregisterThread = module.getOrInsertFunction("lyraGcLeaveWorker", emit.getVoidTy(), emit.getInt32Ty());
        auto getClosure = module.getOrInsertFunction("lyraCallbackClosure", pointer, pointer);
        auto recordError = module.getOrInsertFunction("lyraCallbackRecordError", emit.getVoidTy(), pointer, pointer);
        auto enterShield = module.getOrInsertFunction("lyraCancellationShieldEnter", emit.getVoidTy());
        auto leaveShield = module.getOrInsertFunction("lyraCancellationShieldLeave", emit.getVoidTy());
        auto* registration = emit.CreateCall(registerThread);
        auto* callable = emit.CreateCall(getClosure, {callback});
        std::vector<llvm::Value*> inputs;
        for (unsigned slot = 0; slot < shape.parameters.size(); ++slot) {
            auto param = shape.parameters[slot];
            if (auto attr = extension(param); attr != llvm::Attribute::None) trampoline->addParamAttr(slot + 1, attr);
            auto* argument = trampoline->getArg(slot + 1);
            inputs.push_back(param == Type::Bool ? emit.CreateIsNotNull(argument) : argument);
        }
        if (auto attr = extension(shape.result); attr != llvm::Attribute::None) trampoline->addRetAttr(attr);
        auto* slot = NativeExceptions{emit}.outcomeSlot();
        emit.CreateCall(enterShield);
        auto* value = NativeClosures{emit}.invoke(closureSignature, callable, inputs, nullptr, slot);
        emit.CreateCall(leaveShield);
        auto* error = NativeExceptions{emit}.outcomeError(slot);
        emit.CreateCondBr(emit.CreateIsNotNull(error), failed, finished);
        emit.SetInsertPoint(failed);
        emit.CreateCall(recordError, {callback, error});
        llvm::Value* substitute = nullptr;
        if (shape.result != Type::Unit) {
            auto getFallback = module.getOrInsertFunction("lyraCallbackFallback", pointer, pointer);
            substitute = emit.CreateLoad(value->getType(), emit.CreateCall(getFallback, {callback}));
        }
        emit.CreateBr(finished);
        emit.SetInsertPoint(finished);
        llvm::Value* result = nullptr;
        if (substitute) {
            auto* selected = emit.CreatePHI(value->getType(), 2);
            selected->addIncoming(value, entry);
            selected->addIncoming(substitute, failed);
            result = shape.result == Type::Bool ? emit.CreateZExt(selected, emit.getInt8Ty()) : selected;
        }
        emit.CreateCall(unregisterThread, {registration});
        if (result) emit.CreateRet(result);
        else emit.CreateRetVoid();
    }
    auto registerCallback = module.getOrInsertFunction("lyraCallbackRegister", pointer, pointer, pointer, pointer);
    return builder.CreateCall(registerCallback, {closure, trampoline, fallback});
}

}
