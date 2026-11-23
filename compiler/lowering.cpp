module;
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <unordered_map>
#include <variant>
#include <vector>

module lyra.lowering;
import lyra.check;

namespace lyra {

std::unique_ptr<llvm::Module> lower(const Module& functions, const std::vector<Source>& sources, llvm::LLVMContext& context) {
    auto module = std::make_unique<llvm::Module>("lyra", context);
    module->setTargetTriple("x86_64-pc-linux-gnu");
    llvm::IRBuilder<> builder(context);
    auto intType = builder.getInt64Ty();
    auto unitType = builder.getVoidTy();
    auto print = module->getOrInsertFunction("lyraPrintInt", unitType, intType);
    auto overflow = module->getOrInsertFunction("lyraIntOverflow", unitType, builder.getPtrTy(), intType, intType);
    auto checkedAdd = llvm::Intrinsic::getDeclaration(module.get(), llvm::Intrinsic::sadd_with_overflow, {intType});
    std::unordered_map<std::string, llvm::FunctionCallee> callees{{"print", print}};
    for (const auto& function : functions) {
        auto signature = llvm::FunctionType::get(function.result == Type::Int ? intType : unitType, false);
        callees.emplace(function.name, llvm::Function::Create(signature, llvm::Function::InternalLinkage, "lyra.fn." + function.name, *module));
    }

    for (const auto& function : functions) {
        auto native = llvm::cast<llvm::Function>(callees.at(function.name).getCallee());
        builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", native));
        const auto& source = sources[function.file];
        std::vector<llvm::Value*> values;
        for (const auto& expr : function.expressions) {
            if (auto* number = std::get_if<std::int64_t>(&expr.node)) {
                values.push_back(llvm::ConstantInt::getSigned(intType, *number));
            } else if (auto* call = std::get_if<Call>(&expr.node)) {
                std::vector<llvm::Value*> args;
                for (auto arg : call->arguments) args.push_back(values[arg]);
                values.push_back(builder.CreateCall(callees.at(call->name), args));
            } else {
                const auto& add = std::get<Add>(expr.node);
                auto pair = builder.CreateCall(checkedAdd, {values[add.left], values[add.right]});
                auto failed = llvm::BasicBlock::Create(context, "overflow", native);
                auto next = llvm::BasicBlock::Create(context, "sum", native);
                builder.CreateCondBr(builder.CreateExtractValue(pair, 1), failed, next);
                builder.SetInsertPoint(failed);
                auto position = source.position(add.operatorSpan.begin);
                builder.CreateCall(overflow, {builder.CreateGlobalStringPtr(source.path.string()), builder.getInt64(position.line), builder.getInt64(position.column)});
                builder.CreateUnreachable();
                builder.SetInsertPoint(next);
                values.push_back(builder.CreateExtractValue(pair, 0));
            }
        }
        if (function.result == Type::Unit) builder.CreateRetVoid();
        else builder.CreateRet(values[function.body.back().expression]);
    }

    auto entry = llvm::Function::Create(llvm::FunctionType::get(builder.getInt32Ty(), false), llvm::Function::ExternalLinkage, "main", *module);
    builder.SetInsertPoint(llvm::BasicBlock::Create(context, "entry", entry));
    builder.CreateCall(callees.at("main"));
    builder.CreateRet(builder.getInt32(0));
    std::string reason;
    llvm::raw_string_ostream log(reason);
    if (llvm::verifyModule(*module, &log)) throw Diagnostic{"LYB0001", "编译器生成的 LLVM 模块无效：" + reason};
    return module;
}

}
