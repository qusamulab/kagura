// Audit adapters shared by named passes and the automatic pipeline.
#ifndef KAGURA_AUDITED_PASS_H
#define KAGURA_AUDITED_PASS_H

#include "kagura/Options.h"
#include "kagura/Utils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/raw_ostream.h"

namespace kagura {

inline void recordPassResult(llvm::Module &M, llvm::StringRef Pass,
                             llvm::StringRef Scope, llvm::StringRef Name,
                             bool Changed) {
  auto &C = M.getContext();
  M.getOrInsertNamedMetadata("kagura.audit.results")
      ->addOperand(llvm::MDNode::get(
          C, {llvm::MDString::get(C, Pass), llvm::MDString::get(C, Scope),
              llvm::MDString::get(C, Name),
              llvm::MDString::get(C, Changed ? "changed" : "unchanged")}));
}

// Include referenced global initializers: an in-place wide-string/global
// rewrite need not change the textual body of any consuming function.
inline uint64_t auditFingerprint(const llvm::Module &M) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  M.print(OS, nullptr);
  return fnv1a64(OS.str());
}

inline uint64_t auditFingerprint(const llvm::Function &F) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  F.print(OS);
  llvm::SmallVector<const llvm::Value *, 16> Worklist;
  llvm::SmallPtrSet<const llvm::Value *, 32> Seen;
  for (const auto &BB : F)
    for (const auto &I : BB)
      for (const llvm::Value *V : I.operands())
        Worklist.push_back(V);
  while (!Worklist.empty()) {
    const auto *V = Worklist.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    if (const auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
      GV->print(OS);
      if (GV->hasInitializer())
        Worklist.push_back(GV->getInitializer());
    } else if (const auto *C = llvm::dyn_cast<llvm::Constant>(V)) {
      for (const auto &Op : C->operands())
        Worklist.push_back(Op.get());
    }
  }
  return fnv1a64(OS.str());
}

template <typename PassT>
class AuditedFunctionPass
    : public llvm::PassInfoMixin<AuditedFunctionPass<PassT>> {
  PassT Pass;
  std::string Name;

public:
  AuditedFunctionPass(PassT P, llvm::StringRef N)
      : Pass(std::move(P)), Name(N.str()) {}
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &AM) {
    if (!opt::AuditLog)
      return Pass.run(F, AM);
    uint64_t Before = auditFingerprint(F);
    auto PA = Pass.run(F, AM);
    bool Changed = Before != auditFingerprint(F);
    if (Changed)
      markObfuscated(F, llvm::StringRef(Name).drop_front(7));
    recordPassResult(*F.getParent(), Name, "function", F.getName(), Changed);
    return PA;
  }
};

template <typename PassT>
class AuditedModulePass : public llvm::PassInfoMixin<AuditedModulePass<PassT>> {
  PassT Pass;
  std::string Name;

public:
  AuditedModulePass(PassT P, llvm::StringRef N)
      : Pass(std::move(P)), Name(N.str()) {}
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM) {
    if (!opt::AuditLog)
      return Pass.run(M, AM);
    llvm::DenseMap<const llvm::Function *, uint64_t> Before;
    for (const auto &F : M)
      if (!F.isDeclaration() && !isKaguraSymbol(F.getName()))
        Before[&F] = auditFingerprint(F);
    uint64_t ModuleBefore = auditFingerprint(M);
    auto PA = Pass.run(M, AM);
    bool ModuleChanged = ModuleBefore != auditFingerprint(M);
    for (auto &F : M) {
      auto It = Before.find(&F);
      if (It == Before.end())
        continue;
      bool Changed = It->second != auditFingerprint(F);
      if (Changed)
        markObfuscated(F, llvm::StringRef(Name).drop_front(7));
      recordPassResult(M, Name, "function", F.getName(), Changed);
    }
    recordPassResult(M, Name, "module", M.getName(), ModuleChanged);
    return PA;
  }
};

template <typename P> auto auditedFunction(P Pass, llvm::StringRef Name) {
  return AuditedFunctionPass<P>(std::move(Pass), Name);
}
template <typename P> auto auditedModule(P Pass, llvm::StringRef Name) {
  return AuditedModulePass<P>(std::move(Pass), Name);
}
} // namespace kagura
#endif
