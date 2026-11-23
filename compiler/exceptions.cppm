module;
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

export module lyra.exceptions;
import lyra.source;
import lyra.platform;

export namespace lyra {

struct NativeExceptions {
    llvm::IRBuilder<>& builder;

    llvm::Module& module() const { return *builder.GetInsertBlock()->getModule(); }
    llvm::Constant* none() const { return llvm::ConstantPointerNull::get(builder.getPtrTy()); }

    llvm::AllocaInst* slot() const {
        auto* cell = builder.CreateAlloca(builder.getPtrTy(), nullptr, "exception.slot");
        builder.CreateStore(none(), cell);
        return cell;
    }

    llvm::Value* create(llvm::Value* message) const {
        auto pointer = builder.getPtrTy();
        return builder.CreateCall(module().getOrInsertFunction("lyraExceptionCreate", pointer, pointer), {message});
    }

    llvm::Value* create(llvm::Value* message, llvm::Value* type, llvm::StringRef name) const {
        auto pointer = builder.getPtrTy();
        auto create = module().getOrInsertFunction("lyraExceptionCreateTyped", pointer, pointer, pointer, pointer);
        return builder.CreateCall(create, {message, type, builder.CreateGlobalStringPtr(name)});
    }

    llvm::Value* matches(llvm::Value* error, llvm::Value* type) const {
        auto pointer = builder.getPtrTy();
        auto actual = builder.CreateCall(module().getOrInsertFunction("lyraExceptionType", pointer, pointer), {error});
        return builder.CreateICmpEQ(actual, type);
    }

    llvm::Value* message(llvm::Value* error) const {
        auto pointer = builder.getPtrTy();
        return builder.CreateCall(module().getOrInsertFunction("lyraExceptionMessage", pointer, pointer), {error});
    }

    llvm::Value* same(llvm::Value* left, llvm::Value* right) const {
        auto pointer = builder.getPtrTy();
        auto same = module().getOrInsertFunction("lyraExceptionSame", builder.getInt32Ty(), pointer, pointer);
        return builder.CreateIsNotNull(builder.CreateCall(same, {left, right}));
    }

    llvm::Value* locate(llvm::Value* error, const Source& source, Span site, const char* role = nullptr) const {
        auto pointer = builder.getPtrTy();
        auto integer = builder.getInt64Ty();
        auto position = source.origin ? source.origin->start : source.position(site.begin);
        auto file = builder.CreateGlobalStringPtr(source.origin ? source.origin->file : pathText(source.path));
        auto line = builder.getInt64(position.line), column = builder.getInt64(position.column);
        if (!role) return builder.CreateCall(module().getOrInsertFunction("lyraExceptionRaise", pointer, pointer, pointer, integer, integer), {error, file, line, column});
        return builder.CreateCall(module().getOrInsertFunction("lyraExceptionFrame", pointer, pointer, pointer, pointer, integer, integer), {error, builder.CreateGlobalStringPtr(role), file, line, column});
    }

    void report(llvm::Value* error) const {
        builder.CreateCall(module().getOrInsertFunction("lyraExceptionReport", builder.getVoidTy(), builder.getPtrTy()), {error});
    }

    llvm::Value* created(llvm::Value* task, const Source& source, Span site) const {
        auto pointer = builder.getPtrTy(); auto integer = builder.getInt64Ty();
        auto position = source.origin ? source.origin->start : source.position(site.begin);
        auto file = builder.CreateGlobalStringPtr(source.origin ? source.origin->file : pathText(source.path));
        auto note = module().getOrInsertFunction("lyraTaskCreatedAt", builder.getVoidTy(), pointer, pointer, integer, integer);
        builder.CreateCall(note, {task, file, builder.getInt64(position.line), builder.getInt64(position.column)});
        return task;
    }
};

}
