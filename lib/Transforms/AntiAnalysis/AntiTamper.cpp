//===-- AntiTamper.cpp - Integrity-check injection pass -------------------===//
//
// Pass each eligible function's address and IR opcode fingerprint to
// kagura_runtime_hash_check at entry. The shipped runtime inspects selected
// entry instructions and symbol names. Machine-code hash verification would
// require a trusted checksum produced after code generation and linking.
//
// At main entry, kagura_self_check runs the platform's jailbreak/root probes.
//
// Eligibility
// -----------
// A function is eligible if:
//   - shouldObfuscate(F, "tamper", EnableTamper) returns true, OR
//   - the function has the annotation "kagura.tamper"
// The "kagura.tamper" annotation takes explicit precedence even when the
// global -kagura-tamper flag is off, allowing surgical opt-in from source:
//
//   __attribute__((annotate("kagura.tamper")))
//   void sensitive_function(void) { ... }
//
// Pass key:  "tamper"
// CLI flag:  -kagura-tamper
//
//===----------------------------------------------------------------------===//

#include "kagura/Options.h"
#include "kagura/Passes/AntiAnalysis.h"
#include "kagura/Utils.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <vector>

using namespace llvm;

namespace kagura {

//===----------------------------------------------------------------------===//
// FNV-1a compile-time hash
//===----------------------------------------------------------------------===//

/// Compute an FNV-1a hash over the sequence of instruction opcodes in F.
static uint32_t computeOpcodeHash(const Function &F) {
  uint32_t H = fnv1a32Init();
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      uint32_t OC = static_cast<uint32_t>(I.getOpcode());
      // Feed all four bytes of the opcode word in little-endian order so the
      // hash is portable across host endianness during compilation.
      H = fnv1a32Update(H, static_cast<uint8_t>(OC & 0xFFu));
      H = fnv1a32Update(H, static_cast<uint8_t>((OC >> 8) & 0xFFu));
      H = fnv1a32Update(H, static_cast<uint8_t>((OC >> 16) & 0xFFu));
      H = fnv1a32Update(H, static_cast<uint8_t>((OC >> 24) & 0xFFu));
    }
  }
  return H;
}

//===----------------------------------------------------------------------===//
// IR declaration helpers
//===----------------------------------------------------------------------===//

/// Declare (or look up):
///   void kagura_tamper_detected(void)
static Function *getTamperDetectedFn(Module &M) {
  LLVMContext &Ctx = M.getContext();
  auto *FTy = FunctionType::get(Type::getVoidTy(Ctx), /*isVarArg=*/false);
  Function *F = getOrDeclare(M, "kagura_tamper_detected", FTy);
  // Mark noreturn: once tamper is detected we never return normally.
  F->addFnAttr(Attribute::NoReturn);
  F->addFnAttr(Attribute::NoUnwind);
  return F;
}

/// Declare (or look up):
///   void kagura_self_check(void)
static Function *getSelfCheckFn(Module &M) {
  LLVMContext &Ctx = M.getContext();
  auto *FTy = FunctionType::get(Type::getVoidTy(Ctx), /*isVarArg=*/false);
  Function *F = getOrDeclare(M, "kagura_self_check", FTy);
  F->addFnAttr(Attribute::NoUnwind);
  return F;
}

/// Declare (or look up):
///   void kagura_runtime_hash_check(void *fn_ptr, uint32_t expected_hash)
static Function *getRuntimeHashCheckFn(Module &M) {
  LLVMContext &Ctx = M.getContext();
  auto *VoidTy   = Type::getVoidTy(Ctx);
  auto *PtrTy    = PointerType::getUnqual(Ctx); // opaque ptr (LLVM 16+)
  auto *Int32Ty  = Type::getInt32Ty(Ctx);
  auto *FTy = FunctionType::get(VoidTy, {PtrTy, Int32Ty}, /*isVarArg=*/false);
  Function *F = getOrDeclare(M, "kagura_runtime_hash_check", FTy);
  F->addFnAttr(Attribute::NoUnwind);
  return F;
}

//===----------------------------------------------------------------------===//
// Instrumentation helpers
//===----------------------------------------------------------------------===//

/// Insert `kagura_self_check()` at the very beginning of `main` before any
/// user instructions run. This triggers platform-specific jailbreak/root checks.
static void insertSelfCheckInMain(Function &MainFn, Function *SelfCheckFn) {
  BasicBlock &Entry = MainFn.getEntryBlock();

  // Insert before the first instruction in the entry block so the check runs
  // before any user-visible side effects.
  IRBuilder<> B(&*Entry.getFirstInsertionPt());
  B.CreateCall(SelfCheckFn, {});
}

/// Pass the function address and its IR opcode fingerprint to the runtime.
static void insertHashCheck(Function &F, uint32_t OpcodeHash,
                             Function *HashCheckFn) {
  LLVMContext &Ctx = F.getContext();
  BasicBlock &Entry = F.getEntryBlock();

  Instruction *InsertPt = &*Entry.getFirstInsertionPt();

  IRBuilder<> B(InsertPt);

  // Build the function pointer operand.  In opaque-pointer LLVM the function
  // value is already of pointer type; no bitcast needed.
  Value *FnPtr    = static_cast<Value *>(&F);
  Value *HashCst  = ConstantInt::get(Type::getInt32Ty(Ctx), OpcodeHash);

  B.CreateCall(HashCheckFn, {FnPtr, HashCst});
}

//===----------------------------------------------------------------------===//
// Per-function eligibility
//===----------------------------------------------------------------------===//

/// A function is eligible for tamper protection if:
///   1. It is a definition (not a declaration).
///   2. It is not one of kagura's own injected helpers.
///   3. It passes shouldObfuscate() with the "tamper" key, OR it carries the
///      explicit "kagura.tamper" annotation.
///
/// Note: shouldObfuscate() already returns false for names starting with
/// "kagura_", so the annotation check is the only extra path here.
static bool isTamperEligible(Function &F, bool GlobalFlag) {
  if (F.isDeclaration() || F.isIntrinsic())
    return false;
  // Explicit per-function annotation overrides everything.
  if (hasAnnotation(F, "kagura.tamper"))
    return true;
  return shouldObfuscate(F, "tamper", GlobalFlag);
}

//===----------------------------------------------------------------------===//
// AntiTamperPass::run
//===----------------------------------------------------------------------===//

PreservedAnalyses AntiTamperPass::run(Module &M, ModuleAnalysisManager &) {
  // C.1: Integrity checks call platform runtime functions (jailbreak detection,
  // kagura_self_check) that are not available in the WebAssembly sandbox.
  if (kagura::isWasmTarget(M))
    return PreservedAnalyses::all();

  bool Changed = false;

  // Pre-declare all runtime symbols once so that every injection below can
  // reuse the same declarations without re-scanning the module.
  Function *TamperDetectedFn  = getTamperDetectedFn(M);
  Function *SelfCheckFn       = getSelfCheckFn(M);
  Function *HashCheckFn       = getRuntimeHashCheckFn(M);

  // Suppress unused-variable warnings in builds where we don't generate direct
  // calls to kagura_tamper_detected from the pass (the runtime calls it).
  (void)TamperDetectedFn;

  // Collect functions to instrument.  We snapshot them because insertions into
  // the entry block do not invalidate existing Function* pointers but iterating
  // while modifying the function list would be undefined behaviour.
  std::vector<Function *> Targets;
  Function *MainFn = nullptr;

  for (Function &F : M) {
    if (F.getName() == "main")
      MainFn = &F;
    if (isTamperEligible(F, kagura::opt::Tamper))
      Targets.push_back(&F);
  }

  // -- Step 1: Insert kagura_self_check() into main() -----------------------
  //
  // The process-wide check also runs when main itself is excluded.
  if (MainFn && !MainFn->isDeclaration()) {
    insertSelfCheckInMain(*MainFn, SelfCheckFn);
    Changed = true;
    errs() << "[kagura] anti-tamper: inserted kagura_self_check() in main\n";
  }

  // Compute each IR fingerprint before inserting its entry check.
  uint32_t TotalInstrumented = 0;

  for (Function *F : Targets) {
    uint32_t Hash = computeOpcodeHash(*F);

    errs() << "[kagura] anti-tamper: instrumenting '" << F->getName()
           << "' with opcode hash 0x" << llvm::Twine::utohexstr(Hash) << "\n";

    insertHashCheck(*F, Hash, HashCheckFn);
    ++TotalInstrumented;
    Changed = true;
  }

  if (TotalInstrumented > 0)
    errs() << "[kagura] anti-tamper: instrumented " << TotalInstrumented
           << " function(s)\n";

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace kagura
