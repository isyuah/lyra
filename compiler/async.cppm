module;
#include <cstdint>
#include <span>
#include <vector>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Local.h>

export module lyra.async;

namespace lyra {
namespace {

using Builder = llvm::IRBuilder<>;

llvm::Module& moduleAt(Builder& builder) { return *builder.GetInsertBlock()->getModule(); }

llvm::FunctionCallee runtime(Builder& builder, llvm::StringRef name, llvm::Type* result, std::initializer_list<llvm::Type*> inputs) {
    return moduleAt(builder).getOrInsertFunction(name, llvm::FunctionType::get(result, inputs, false));
}

llvm::Value* allocate(Builder& builder, llvm::Type* type) {
    return builder.CreateCall(runtime(builder, "lyraGcAlloc", builder.getPtrTy(), {builder.getInt64Ty()}), {llvm::ConstantExpr::getSizeOf(type)});
}

llvm::CallInst* marker(Builder& builder, llvm::StringRef name, llvm::Type* type, llvm::Value* input) {
    auto& module = moduleAt(builder);
    auto signature = llvm::FunctionType::get(type, {input->getType()}, false);
    auto* declaration = llvm::Function::Create(signature, llvm::Function::ExternalLinkage, name, module);
    declaration->addFnAttr("lyra.suspension", name);
    return builder.CreateCall(declaration, {input});
}

void eraseMarker(llvm::CallInst* marker) {
    auto* declaration = marker->getCalledFunction();
    marker->eraseFromParent();
    declaration->eraseFromParent();
}

struct Site {
    llvm::CallInst* instruction;
    llvm::BasicBlock* resume;
    llvm::BasicBlock* read;
};

}

export llvm::Function* asyncBody(llvm::Function& factory, llvm::Type* result) {
    auto signature = llvm::FunctionType::get(result, factory.getFunctionType()->params(), false);
    return llvm::Function::Create(signature, llvm::Function::InternalLinkage, factory.getName() + ".body", factory.getParent());
}

export llvm::Value* asyncAwait(llvm::IRBuilder<>& builder, llvm::Type* result, llvm::Value* task) {
    return marker(builder, "lyra.await", result, task);
}

export llvm::Value* asyncSpawn(llvm::IRBuilder<>& builder, llvm::Value* task) {
    return marker(builder, "lyra.spawn", builder.getPtrTy(), task);
}

export llvm::Value* asyncEntry(llvm::IRBuilder<>& builder, llvm::Value* coldRoot, llvm::Type* resultType) {
    auto* status = builder.CreateCall(runtime(builder, "lyraTaskRunRoot", builder.getInt32Ty(), {builder.getPtrTy()}), {coldRoot});
    if (!resultType) return status;
    auto* start = builder.GetInsertBlock();
    auto* main = start->getParent();
    auto* check = llvm::BasicBlock::Create(builder.getContext(), "root.result", main);
    auto* error = llvm::BasicBlock::Create(builder.getContext(), "root.error", main);
    auto* done = llvm::BasicBlock::Create(builder.getContext(), "root.done", main);
    builder.CreateCondBr(builder.CreateIsNull(status), check, done);
    builder.SetInsertPoint(check);
    auto* box = builder.CreateCall(runtime(builder, "lyraTaskOutcome", builder.getPtrTy(), {builder.getPtrTy()}), {coldRoot});
    auto* result = builder.CreateLoad(resultType, box);
    builder.CreateCondBr(builder.CreateExtractValue(result, 0), done, error);
    builder.SetInsertPoint(error);
    auto* failed = builder.CreateCall(runtime(builder, "lyraReportStringError", builder.getInt32Ty(), {builder.getPtrTy()}), {builder.CreateExtractValue(result, 2)});
    builder.CreateBr(done);
    builder.SetInsertPoint(done);
    auto* exit = builder.CreatePHI(builder.getInt32Ty(), 3);
    exit->addIncoming(status, start);
    exit->addIncoming(status, check);
    exit->addIncoming(failed, error);
    return exit;
}

export llvm::Value* asyncCell(llvm::IRBuilder<>& builder, llvm::Type* type) { return allocate(builder, type); }

namespace {

llvm::AllocaInst* scratch(Builder& builder, llvm::Type* type) {
    auto& entry = builder.GetInsertBlock()->getParent()->getEntryBlock();
    Builder prologue(&entry, entry.getFirstInsertionPt());
    return prologue.CreateAlloca(type);
}

void sharedCopy(Builder& builder, llvm::Value* destination, llvm::Value* source, llvm::Type* type) {
    auto* pointer = builder.getPtrTy();
    builder.CreateCall(runtime(builder, "lyraTaskSharedCopy", builder.getVoidTy(), {pointer, pointer, builder.getInt64Ty()}), {destination, source, llvm::ConstantExpr::getSizeOf(type)});
}

}

export llvm::Value* sharedRead(llvm::IRBuilder<>& builder, llvm::Type* type, llvm::Value* address) {
    auto* cell = scratch(builder, type);
    sharedCopy(builder, cell, address, type);
    return builder.CreateLoad(type, cell);
}

export void sharedWrite(llvm::IRBuilder<>& builder, llvm::Value* value, llvm::Value* address) {
    auto* cell = scratch(builder, value->getType());
    builder.CreateStore(value, cell);
    sharedCopy(builder, address, cell, value->getType());
}

export struct BlockingCode {
    llvm::Function* body;
    llvm::StructType* environment;
};

export BlockingCode blockingCode(llvm::Function& owner, llvm::Type* result, std::size_t captures) {
    auto* pointer = llvm::PointerType::getUnqual(owner.getContext());
    auto* signature = llvm::FunctionType::get(result, std::vector<llvm::Type*>(captures, pointer), false);
    auto* body = llvm::Function::Create(signature, llvm::Function::InternalLinkage, owner.getName() + ".worker", owner.getParent());
    auto* environment = llvm::StructType::create(owner.getContext(), signature->params(), body->getName().str() + ".env");
    return {body, environment};
}

export llvm::Value* blockingTask(llvm::IRBuilder<>& builder, BlockingCode code, std::span<llvm::Value* const> cells) {
    auto* pointer = builder.getPtrTy();
    auto* captured = allocate(builder, code.environment);
    for (std::size_t index = 0; index < cells.size(); ++index) builder.CreateStore(cells[index], builder.CreateStructGEP(code.environment, captured, index));
    auto* thunk = llvm::Function::Create(llvm::FunctionType::get(pointer, {pointer}, false), llvm::Function::InternalLinkage, code.body->getName() + ".call", &moduleAt(builder));
    Builder worker(llvm::BasicBlock::Create(builder.getContext(), "capture", thunk));
    std::vector<llvm::Value*> args;
    for (std::size_t index = 0; index < cells.size(); ++index) args.push_back(worker.CreateLoad(pointer, worker.CreateStructGEP(code.environment, thunk->getArg(0), index)));
    auto* value = worker.CreateCall(code.body, args);
    auto* boxed = allocate(worker, value->getType());
    worker.CreateStore(value, boxed);
    worker.CreateRet(boxed);
    return builder.CreateCall(runtime(builder, "lyraTaskBlocking", pointer, {pointer, pointer}), {thunk, captured});
}

export void lowerAsync(llvm::Function& factory, llvm::Function& body) {
    auto& context = factory.getContext();
    auto& module = *factory.getParent();
    Builder builder(context);
    auto* pointer = builder.getPtrTy();
    auto* noResult = builder.getVoidTy();
    auto* integer = builder.getInt32Ty();
    std::vector<llvm::CallInst*> waits, spawns;
    std::vector<llvm::PHINode*> phis;
    std::vector<llvm::ReturnInst*> returns;
    for (auto& instruction : llvm::instructions(body)) {
        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction)) {
            auto* callee = call->getCalledFunction();
            if (callee && callee->hasFnAttribute("lyra.suspension")) {
                auto name = callee->getFnAttribute("lyra.suspension").getValueAsString();
                (name == "lyra.await" ? waits : spawns).push_back(call);
            }
        } else if (auto* phi = llvm::dyn_cast<llvm::PHINode>(&instruction)) phis.push_back(phi);
        else if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&instruction)) returns.push_back(ret);
    }

    std::vector<Site> sites;
    for (auto* wait : waits) {
        auto* resume = wait->getParent()->splitBasicBlock(wait, "await.check");
        auto* read = resume->splitBasicBlock(wait->getNextNode(), "await.value");
        sites.push_back({wait, resume, read});
    }
    auto* allocaPoint = &*body.getEntryBlock().getFirstInsertionPt();
    for (auto* phi : phis) llvm::DemotePHIToStack(phi, allocaPoint);
    std::vector<llvm::Instruction*> spills;
    for (auto& instruction : llvm::instructions(body)) {
        if (!llvm::isa<llvm::AllocaInst>(instruction) && !instruction.getType()->isVoidTy() && instruction.isUsedOutsideOfBlock(instruction.getParent()))
            spills.push_back(&instruction);
    }
    for (auto* value : spills) llvm::DemoteRegToStack(*value, false, allocaPoint);

    std::vector<llvm::Type*> fields{integer};
    for (auto& parameter : factory.args()) fields.push_back(parameter.getType());
    std::vector<llvm::AllocaInst*> slots;
    for (auto& instruction : llvm::instructions(body)) if (auto* slot = llvm::dyn_cast<llvm::AllocaInst>(&instruction)) {
        assert(slot->isStaticAlloca() && !slot->isArrayAllocation());
        slots.push_back(slot);
        fields.push_back(slot->getAllocatedType());
    }
    auto* frameType = llvm::StructType::create(context, fields, factory.getName().str() + ".frame");
    auto* step = llvm::Function::Create(llvm::FunctionType::get(noResult, {pointer, pointer}, false), llvm::Function::InternalLinkage, factory.getName() + ".step", module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "create.cold", &factory));
    auto* captured = allocate(builder, frameType);
    for (auto& parameter : factory.args()) builder.CreateStore(&parameter, builder.CreateStructGEP(frameType, captured, 1 + parameter.getArgNo()));
    builder.CreateRet(builder.CreateCall(runtime(builder, "lyraTaskCreate", pointer, {pointer, pointer}), {step, captured}));

    auto* dispatch = llvm::BasicBlock::Create(context, "dispatch", step);
    auto* select = llvm::BasicBlock::Create(context, "resume", step);
    auto* cancelled = llvm::BasicBlock::Create(context, "cancelled", step);
    auto* yield = llvm::BasicBlock::Create(context, "yield", step);
    auto* invalid = llvm::BasicBlock::Create(context, "invalid.pc", step);
    auto* initial = &body.getEntryBlock();
    step->splice(step->end(), &body);
    builder.SetInsertPoint(dispatch);
    auto* task = step->getArg(0);
    auto* frame = step->getArg(1);
    auto* pc = builder.CreateStructGEP(frameType, frame, 0);
    for (auto& parameter : body.args()) {
        auto* source = builder.CreateStructGEP(frameType, frame, 1 + parameter.getArgNo());
        parameter.replaceAllUsesWith(builder.CreateLoad(parameter.getType(), source));
    }
    unsigned field = 1 + factory.arg_size();
    for (auto* slot : slots) {
        slot->replaceAllUsesWith(builder.CreateStructGEP(frameType, frame, field++));
        slot->eraseFromParent();
    }
    auto* cancellation = builder.CreateCall(runtime(builder, "lyraTaskCancelRequested", integer, {pointer}), {task});
    builder.CreateCondBr(builder.CreateIsNotNull(cancellation), cancelled, select);
    builder.SetInsertPoint(cancelled);
    builder.CreateCall(runtime(builder, "lyraTaskFinishCancelled", noResult, {pointer}), {task});
    builder.CreateRetVoid();
    builder.SetInsertPoint(yield);
    builder.CreateRetVoid();
    builder.SetInsertPoint(invalid);
    builder.CreateUnreachable();
    builder.SetInsertPoint(select);
    auto* branch = builder.CreateSwitch(builder.CreateLoad(integer, pc), invalid, sites.size() + 1);
    branch->addCase(builder.getInt32(0), initial);

    unsigned state = 0;
    for (auto site : sites) {
        auto* wait = site.instruction;
        auto* target = wait->getArgOperand(0);
        site.resume->getTerminator()->eraseFromParent();
        std::vector<llvm::Instruction*> after;
        for (auto* next = wait->getNextNode(); next; next = next->getNextNode()) after.push_back(next);
        for (auto cursor = after.rbegin(); cursor != after.rend(); ++cursor) (*cursor)->moveBefore(&*site.read->getFirstInsertionPt());
        builder.SetInsertPoint(wait);
        auto* index = builder.getInt32(++state);
        branch->addCase(index, site.resume);
        builder.CreateStore(index, pc);
        auto* ready = builder.CreateCall(runtime(builder, "lyraTaskAwaitValue", integer, {pointer, pointer}), {task, target});
        builder.CreateCondBr(builder.CreateIsNotNull(ready), site.read, yield);
        builder.SetInsertPoint(&*site.read->getFirstInsertionPt());
        auto* box = builder.CreateCall(runtime(builder, "lyraTaskOutcome", pointer, {pointer}), {target});
        wait->replaceAllUsesWith(builder.CreateLoad(wait->getType(), box));
        eraseMarker(wait);
    }
    for (auto* spawn : spawns) {
        builder.SetInsertPoint(spawn);
        spawn->replaceAllUsesWith(builder.CreateCall(runtime(builder, "lyraTaskSpawn", pointer, {pointer, pointer}), {task, spawn->getArgOperand(0)}));
        eraseMarker(spawn);
    }
    for (auto* ret : returns) {
        builder.SetInsertPoint(ret);
        auto* value = ret->getReturnValue();
        auto* box = allocate(builder, value->getType());
        builder.CreateStore(value, box);
        builder.CreateCall(runtime(builder, "lyraTaskComplete", noResult, {pointer, pointer}), {task, box});
        builder.CreateRetVoid();
        ret->eraseFromParent();
    }
    body.eraseFromParent();
}

}
