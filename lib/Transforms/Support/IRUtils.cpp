//===-- IRUtils.cpp - Shared IR helpers -----------------------------------===//

#include "kagura/Utils.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;

namespace kagura {

// ---- Kagura-generated IR recognition ---------------------------------------

static constexpr StringRef kGeneratedMD = "kagura.generated";

void markGenerated(Instruction &I) {
  I.setMetadata(kGeneratedMD, MDNode::get(I.getContext(), {}));
}

void markGenerated(BasicBlock &BB) {
  for (Instruction &I : BB)
    markGenerated(I);
}

bool isGenerated(const Instruction &I) {
  return I.getMetadata(kGeneratedMD) != nullptr;
}

bool isGenerated(const BasicBlock &BB) {
  for (const Instruction &I : BB)
    if (isGenerated(I))
      return true;
  return false;
}

// ---- Integer width helpers ------------------------------------------------

uint64_t maskForWidth(unsigned Bits) {
  return Bits >= 64 ? ~0ULL : ((1ULL << Bits) - 1);
}

bool isSupportedIntWidth(unsigned Bits) {
  return Bits == 8 || Bits == 16 || Bits == 32 || Bits == 64;
}

bool isSupportedIntType(const Type *Ty) {
  return Ty->isIntegerTy() && isSupportedIntWidth(Ty->getIntegerBitWidth());
}

// ---- Module constructors --------------------------------------------------

void appendKaguraCtor(Module &M, Function *Ctor, CtorPriority P) {
  appendToGlobalCtors(M, Ctor, static_cast<int>(P));
}

// ---- Exception-handling safety ----

bool hasExceptionHandling(const Function &F) {
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB)
      if (isa<InvokeInst>(I) || isa<LandingPadInst>(I) || isa<ResumeInst>(I) ||
          isa<CleanupPadInst>(I) || isa<CatchPadInst>(I) ||
          isa<CatchSwitchInst>(I) || isa<CatchReturnInst>(I) ||
          isa<CleanupReturnInst>(I))
        return true;
  return false;
}

bool isEHBlock(const BasicBlock &BB) {
  const Instruction *First = &BB.front();
  if (isa<LandingPadInst>(First) || isa<CleanupPadInst>(First) ||
      isa<CatchPadInst>(First))
    return true;
  for (const Instruction &I : BB)
    if (isa<LandingPadInst>(I) || isa<CleanupPadInst>(I) ||
        isa<CatchPadInst>(I) || isa<CatchSwitchInst>(I) || isa<ResumeInst>(I))
      return true;
  return false;
}

// ---- IR helpers ----

void demotePhis(Function &F) {
  // Full reg2mem: demote both PHI nodes and any non-PHI instruction whose
  // uses cross a basic-block boundary.  FLA rewires the entire CFG into a
  // switch dispatch, so the old dominator tree is invalidated — every
  // cross-block SSA use would violate dominance in the new CFG.
  //
  // Process PHIs first (they must be demoted before the instructions that
  // use their results, otherwise DemoteRegToStack trips on a PHI user).
  std::vector<PHINode *> Phis;
  for (auto &BB : F)
    for (auto &I : BB)
      if (auto *Phi = dyn_cast<PHINode>(&I))
        Phis.push_back(Phi);
  for (auto *Phi : Phis)
    DemotePHIToStack(Phi);

  // Now demote non-PHI instructions with cross-block uses.
  // Skip terminators, instructions with no uses, and entry allocas,
  // and any instruction in the entry block whose only users are in the same
  // block (avoids demoting entry-block stores/loads that reference args).
  std::vector<Instruction *> ToSpill;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (isa<PHINode>(I) || I.isTerminator() || I.use_empty())
        continue;
      // Entry allocas still dominate the dispatcher. A pointer produced by
      // an alloca in another block needs spilling just like any SSA value.
      if (isa<AllocaInst>(I) && &BB == &F.getEntryBlock())
        continue;
      for (auto *U : I.users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (UI && UI->getParent() != &BB) {
          ToSpill.push_back(&I);
          break;
        }
      }
    }
  }
  for (auto *I : ToSpill)
    DemoteRegToStack(*I, /*VolatileLoads=*/false);
}

std::vector<BasicBlock *> getBlocks(Function &F) {
  std::vector<BasicBlock *> Blocks;
  for (auto &BB : F)
    Blocks.push_back(&BB);
  return Blocks;
}

Function *getOrDeclare(Module &M, StringRef Name, FunctionType *FTy) {
  if (auto *F = M.getFunction(Name))
    return F;
  return Function::Create(FTy, Function::ExternalLinkage, Name, M);
}

Function *createCtorFunction(Module &M, const Twine &Name) {
  LLVMContext &Ctx = M.getContext();
  auto *FTy = FunctionType::get(Type::getVoidTy(Ctx), /*isVarArg=*/false);
  auto *F = Function::Create(FTy, Function::InternalLinkage, Name, M);
  BasicBlock::Create(Ctx, "entry", F);
  return F;
}

// ---- Call rewriting ----

CallInst *replaceCalleeWith(CallInst *CI, FunctionType *FTy, Value *NewCallee) {
  SmallVector<Value *, 8> Args(CI->args());
  SmallVector<OperandBundleDef, 2> Bundles;
  CI->getOperandBundlesAsDefs(Bundles);

  IRBuilder<> B(CI);
  CallInst *NewCI = B.CreateCall(FTy, NewCallee, Args, Bundles);

  NewCI->setCallingConv(CI->getCallingConv());
  NewCI->setAttributes(CI->getAttributes());
  NewCI->setTailCallKind(CI->getTailCallKind());
  NewCI->setDebugLoc(CI->getDebugLoc());
  // Only meaningful on floating-point calls, but harmless otherwise — and
  // omitting it is precisely the drift this helper exists to prevent.
  if (isa<FPMathOperator>(NewCI))
    NewCI->copyFastMathFlags(CI);

  CI->replaceAllUsesWith(NewCI);
  CI->eraseFromParent();
  return NewCI;
}

void replaceAllUsesExcept(Value *Old, Instruction *New) {
  Old->replaceUsesWithIf(New, [&](Use &U) { return U.getUser() != New; });
}

// ---- Constant builders ----

Constant *buildByteArrayConstant(LLVMContext &Ctx, ArrayRef<uint8_t> Data) {
  auto *Int8Ty = Type::getInt8Ty(Ctx);
  auto *ArrTy = ArrayType::get(Int8Ty, Data.size());
  std::vector<Constant *> Bytes;
  Bytes.reserve(Data.size());
  for (uint8_t B : Data)
    Bytes.push_back(ConstantInt::get(Int8Ty, B));
  return ConstantArray::get(ArrTy, Bytes);
}

GlobalVariable *createPrivateByteGlobal(Module &M, ArrayRef<uint8_t> Data,
                                        StringRef Name, bool IsConstant) {
  LLVMContext &Ctx = M.getContext();
  auto *Init = buildByteArrayConstant(Ctx, Data);
  return new GlobalVariable(M, Init->getType(), IsConstant,
                            GlobalValue::PrivateLinkage, Init, Name);
}

} // namespace kagura
