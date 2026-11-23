module;
#include <memory>
#include <vector>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

export module lyra.lowering;
import lyra.check;

export namespace lyra {

std::unique_ptr<llvm::Module> lower(const Module& functions, const std::vector<Source>& sources, llvm::LLVMContext& context);
}
