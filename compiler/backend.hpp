#pragma once

#include "frontend.hpp"
#include <memory>

namespace llvm { class LLVMContext; class Module; }

namespace lyra {

std::unique_ptr<llvm::Module> lower(const Module& functions, const std::vector<Source>& sources, llvm::LLVMContext& context);
void build(const Module& functions, const std::vector<Source>& sources, const std::filesystem::path& output, bool release);

}
