//===-- Passes/ABI.h - C++ ABI protection passes ---------------------------===//
//
// Passes that operate on C++ ABI artefacts — the RTTI name strings and vtables
// the Itanium and MSVC ABIs emit — rather than on a function's own code.
// Sources in lib/Transforms/ABI/.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/IR/PassManager.h"

namespace kagura {

/// Encrypts supported Itanium and MSVC RTTI names. Records the symbol name
/// and entry count of array-initialized vtables for IR inspection.
struct VTableProtectionPass
    : public llvm::PassInfoMixin<VTableProtectionPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                               llvm::ModuleAnalysisManager &MAM);
  static bool isRequired() { return false; }
};

} // namespace kagura
