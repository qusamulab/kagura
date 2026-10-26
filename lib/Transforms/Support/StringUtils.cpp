//===-- StringUtils.cpp - String selection and decryption -----------------===//

#include "kagura/Options.h"
#include "kagura/Utils.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ModRef.h"

using namespace llvm;

namespace kagura {

// ---- Global-variable use analysis ----

bool hasOnlyGuardableUses(const GlobalVariable *GV) {
  if (!GV->hasLocalLinkage() || GV->isThreadLocal() || GV->hasSection() ||
      GV->getAddressSpace() != 0 || GV->isExternallyInitialized() ||
      GV->getName().starts_with("llvm.") || isKaguraSymbol(GV->getName()))
    return false;
  SmallVector<const User *, 16> Worklist(GV->users());
  SmallPtrSet<const User *, 16> Seen;
  while (!Worklist.empty()) {
    const User *U = Worklist.pop_back_val();
    if (!Seen.insert(U).second)
      continue;
    if (const auto *I = dyn_cast<Instruction>(U)) {
      // Funclet calls need bundles and EH pads cannot be split. Decline the
      // whole string before mutation when a safe guard cannot be emitted.
      if (isa<PHINode>(I) || I->isEHPad() ||
          hasExceptionHandling(*I->getFunction()))
        return false;
      continue;
    }
    if (isa<ConstantExpr>(U)) {
      Worklist.append(U->user_begin(), U->user_end());
      continue;
    }
    return false;
  }
  return true;
}

void emitStringGuards(GlobalVariable *GV, GlobalVariable *FlagGV,
                      Function *DecryptStub, StringRef BBPrefix) {
  // Snapshot all instruction leaves; skip the decryptor itself below.
  SmallVector<User *, 16> Worklist(GV->users());
  SmallPtrSet<User *, 16> Seen;
  SmallVector<Instruction *, 16> Users;
  while (!Worklist.empty()) {
    User *U = Worklist.pop_back_val();
    if (!Seen.insert(U).second)
      continue;
    if (auto *I = dyn_cast<Instruction>(U))
      Users.push_back(I);
    else if (isa<ConstantExpr>(U))
      Worklist.append(U->user_begin(), U->user_end());
  }
  for (Instruction *I : Users)
    if (I->getFunction() != DecryptStub)
      emitLazyGuard(I, FlagGV, DecryptStub, BBPrefix);
}

// ---- Lazy-decrypt guard ----

void invalidateMemoryEffects(Function &F) {
  // Lazy initialization introduces writes into functions previously inferred
  // memory(none) or readonly. Propagate to callers, including site attributes,
  // so a later optimizer cannot delete the initialization as side-effect free.
  SmallVector<Function *, 16> Worklist{&F};
  SmallPtrSet<Function *, 16> Seen;
  while (!Worklist.empty()) {
    Function *Current = Worklist.pop_back_val();
    if (!Seen.insert(Current).second)
      continue;
    Current->setMemoryEffects(MemoryEffects::unknown());
    Current->removeFnAttr(Attribute::NoSync);
    Current->removeFnAttr(Attribute::NoFree);
    for (User *U : Current->users())
      if (auto *CB = dyn_cast<CallBase>(U)) {
        CB->setMemoryEffects(MemoryEffects::unknown());
        CB->removeFnAttr(Attribute::NoSync);
        CB->removeFnAttr(Attribute::NoFree);
        Worklist.push_back(CB->getFunction());
      }
  }
}

void emitLazyGuard(Instruction *InsertBefore, GlobalVariable *FlagGV,
                   Function *DecryptStub, StringRef BBPrefix) {
  Function *ParentF = InsertBefore->getFunction();
  invalidateMemoryEffects(*ParentF);
  LLVMContext &Ctx = ParentF->getContext();
  auto *Int8Ty = Type::getInt8Ty(Ctx);

  // Split before the use, then claim initialization or wait for its owner.
  BasicBlock *CheckBB = InsertBefore->getParent();
  BasicBlock *MergeBB =
      CheckBB->splitBasicBlock(InsertBefore, BBPrefix + ".merge");
  BasicBlock *TryBB =
      BasicBlock::Create(Ctx, BBPrefix + ".claim", ParentF, MergeBB);
  BasicBlock *DecryptBB =
      BasicBlock::Create(Ctx, BBPrefix + ".decrypt", ParentF, MergeBB);
  BasicBlock *WaitBB =
      BasicBlock::Create(Ctx, BBPrefix + ".wait", ParentF, MergeBB);

  // States: 0 = uninitialized, 1 = owned by a decryptor, 2 = ready.
  // Acquire/release publishes every decrypted byte to all consuming threads.
  CheckBB->getTerminator()->eraseFromParent();
  IRBuilder<> B(CheckBB);
  auto *Flag = B.CreateLoad(Int8Ty, FlagGV, BBPrefix + ".flag");
  Flag->setAtomic(AtomicOrdering::Acquire);
  B.CreateCondBr(B.CreateICmpEQ(Flag, B.getInt8(2)), MergeBB, TryBB);

  IRBuilder<> TB(TryBB);
  auto *Claim = TB.CreateAtomicCmpXchg(FlagGV, TB.getInt8(0), TB.getInt8(1),
                                       MaybeAlign(1), AtomicOrdering::Acquire,
                                       AtomicOrdering::Monotonic);
  TB.CreateCondBr(TB.CreateExtractValue(Claim, 1), DecryptBB, WaitBB);

  IRBuilder<> DB(DecryptBB);
  DB.CreateCall(DecryptStub);
  DB.CreateStore(DB.getInt8(2), FlagGV)->setAtomic(AtomicOrdering::Release);
  DB.CreateBr(MergeBB);

  IRBuilder<> WB(WaitBB);
  auto *Ready = WB.CreateLoad(Int8Ty, FlagGV, BBPrefix + ".ready");
  Ready->setAtomic(AtomicOrdering::Acquire);
  WB.CreateCondBr(WB.CreateICmpEQ(Ready, WB.getInt8(2)), MergeBB, WaitBB);
}

// ---- String global collection ----

bool canProtectStringAtStartup(const GlobalVariable &GV) {
  if (!GV.hasLocalLinkage() || GV.isThreadLocal() || GV.hasSection() ||
      GV.isExternallyInitialized() || GV.getAddressSpace() != 0 ||
      GV.getName().starts_with("llvm.") || isKaguraSymbol(GV.getName()) ||
      GV.use_empty())
    return false;

  // A constant expression/aggregate has no insertion point of its own.
  // Startup decryption preserves the pointers inside it, including offsets.
  // Do not rewrite strings consumed by the compiler or platform loader.
  SmallVector<const User *, 16> Worklist(GV.users());
  SmallPtrSet<const User *, 32> Seen;
  while (!Worklist.empty()) {
    const User *U = Worklist.pop_back_val();
    if (!Seen.insert(U).second)
      continue;
    if (auto *I = dyn_cast<Instruction>(U)) {
      if (auto *CB = dyn_cast<CallBase>(I))
        if (const Function *F = CB->getCalledFunction()) {
          StringRef N = F->getName();
          if (N.starts_with("llvm.var.annotation") ||
              N.starts_with("llvm.ptr.annotation") ||
              N.starts_with("llvm.annotation"))
            return false;
        }
      continue;
    }
    if (auto *G = dyn_cast<GlobalVariable>(U)) {
      if (G->getName().starts_with("llvm.") || isKaguraSymbol(G->getName()) ||
          G->hasSection())
        return false;
    } else if (!isa<ConstantExpr, ConstantAggregate>(U)) {
      return false;
    }
    Worklist.append(U->user_begin(), U->user_end());
  }
  return true;
}

std::vector<GlobalVariable *> collectStringGlobals(Module &M,
                                                   bool StrictLinkage) {
  std::vector<GlobalVariable *> Result;
  for (auto &GV : M.globals()) {
    if (!GV.isConstant() || !GV.hasInitializer())
      continue;
    if (StrictLinkage && GV.getLinkage() != GlobalValue::PrivateLinkage &&
        GV.getLinkage() != GlobalValue::InternalLinkage)
      continue;
    auto *CDA = dyn_cast<ConstantDataArray>(GV.getInitializer());
    if (!CDA || !CDA->isString())
      continue;
    StringRef S = CDA->getAsString();
    if (opt::StringAggressive) {
      if (!S.empty() && canProtectStringAtStartup(GV))
        Result.push_back(&GV);
      continue;
    }
    if (S.size() < 4)
      continue;
    if (S.contains('%') || S.trim().empty())
      continue;
    bool UsedInFunction = false;
    for (auto *U : GV.users()) {
      if (isa<Instruction>(U) ||
          (isa<ConstantExpr>(U) && !cast<ConstantExpr>(U)->user_empty())) {
        UsedInFunction = true;
        break;
      }
    }
    if (UsedInFunction)
      Result.push_back(&GV);
  }
  return Result;
}

} // namespace kagura
