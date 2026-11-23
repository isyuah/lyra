module;
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <vector>

export module lyra.closures;
import lyra.source;

export namespace lyra {

struct NativeClosures {
    llvm::IRBuilder<>& builder;
    using Value = llvm::Value*;

    llvm::Module& module() const { return *builder.GetInsertBlock()->getModule(); }
    llvm::StructType* layout(std::size_t captures) const {
        return llvm::StructType::get(builder.getContext(), std::vector<llvm::Type*>(captures + 1, builder.getPtrTy()));
    }
    llvm::FunctionType* signature(llvm::Type* result, llvm::ArrayRef<llvm::Type*> parameters, bool returnsTask) const {
        std::vector<llvm::Type*> types{builder.getPtrTy()};
        types.insert(types.end(), parameters.begin(), parameters.end());
        if (returnsTask) types.push_back(builder.getPtrTy());
        types.push_back(builder.getPtrTy());
        return llvm::FunctionType::get(result, types, false);
    }
    llvm::Function* function(llvm::FunctionType* type, const llvm::Twine& name) const {
        return llvm::Function::Create(type, llvm::Function::InternalLinkage, name, module());
    }
    Value close(llvm::Function* code, llvm::ArrayRef<Value> cells) const {
        auto* type = layout(cells.size());
        if (cells.empty()) return new llvm::GlobalVariable(module(), type, true, llvm::GlobalValue::PrivateLinkage,
            llvm::ConstantStruct::get(type, {code}), code->getName() + ".closure");
        auto allocate = module().getOrInsertFunction("lyraGcAlloc", builder.getPtrTy(), builder.getInt64Ty());
        auto* record = builder.CreateCall(allocate, {llvm::ConstantExpr::getSizeOf(type)});
        builder.CreateStore(code, record);
        for (std::size_t index = 0; index < cells.size(); ++index) builder.CreateStore(cells[index], builder.CreateStructGEP(type, record, index + 1));
        return record;
    }
    Value capture(Value record, std::size_t count, std::size_t index) const {
        return builder.CreateLoad(builder.getPtrTy(), builder.CreateStructGEP(layout(count), record, index + 1));
    }
    Value location(const Source& source, Span site) const {
        auto position = source.origin ? source.origin->start : source.position(site.begin);
        auto* file = builder.CreateGlobalStringPtr(source.origin ? source.origin->file : source.path.string());
        auto* point = llvm::ConstantStruct::getAnon({file, builder.getInt64(position.line), builder.getInt64(position.column)});
        return new llvm::GlobalVariable(module(), point->getType(), true, llvm::GlobalValue::PrivateLinkage, point, "closure.site");
    }
    Value invoke(llvm::FunctionType* type, Value record, llvm::ArrayRef<Value> arguments, Value location, Value error) const {
        std::vector<Value> inputs{record};
        inputs.insert(inputs.end(), arguments.begin(), arguments.end());
        if (location) inputs.push_back(location);
        inputs.push_back(error);
        return builder.CreateCall(type, builder.CreateLoad(builder.getPtrTy(), record), inputs);
    }
    Value named(llvm::Function* target, llvm::FunctionType* type, std::size_t count, bool async, bool foreign) const {
        auto name = (target->getName() + ".as.value").str();
        if (auto* record = module().getNamedGlobal(name + ".closure")) return record;
        auto* adapter = function(type, name);
        llvm::IRBuilder<> thunk(llvm::BasicBlock::Create(builder.getContext(), "entry", adapter));
        std::vector<Value> inputs;
        for (std::size_t index = 0; index < count; ++index) inputs.push_back(adapter->getArg(index + 1));
        if (!async && !foreign) inputs.push_back(adapter->getArg(adapter->arg_size() - 1));
        auto* result = thunk.CreateCall(target, inputs);
        if (async) {
            auto pointer = thunk.getPtrTy(); auto integer = thunk.getInt64Ty();
            auto* site = llvm::StructType::get(builder.getContext(), {pointer, integer, integer});
            auto* at = adapter->getArg(count + 1);
            auto note = module().getOrInsertFunction("lyraTaskCreatedAt", thunk.getVoidTy(), pointer, pointer, integer, integer);
            thunk.CreateCall(note, {result, thunk.CreateLoad(pointer, at), thunk.CreateLoad(integer, thunk.CreateStructGEP(site, at, 1)), thunk.CreateLoad(integer, thunk.CreateStructGEP(site, at, 2))});
        }
        thunk.CreateRet(result);
        return close(adapter, {});
    }
};

}
