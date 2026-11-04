//===-- StringUtils.cpp - String selection and decryption -----------------===//

#include "kagura/Options.h"
#include "kagura/Utils.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/ModRef.h"

using namespace llvm;

namespace kagura {

// A scoped literal must have no observable use beyond one call's duration.
// In particular, rejecting global initializers, PHIs, pointer comparisons and
// escaping/returned pointers preserves the normal persistent-literal contract.
static bool refersToString(const Value *V, const GlobalVariable *GV) {
  while (auto *CE = dyn_cast<ConstantExpr>(V)) {
    if (CE->getOpcode() != Instruction::GetElementPtr &&
        CE->getOpcode() != Instruction::BitCast)
      return false;
    V = CE->getOperand(0);
  }
  return V == GV;
}

// MSVC pools literals into discardable COMDAT definitions rather than private
// globals. An unnamed-address, dso-local linkonce_odr literal can disappear
// once every reference in this module has been replaced with a scoped buffer.
static bool scopedStorage(const GlobalVariable *GV) {
  return GV->hasLocalLinkage() ||
         (isWindowsTarget(*GV->getParent()) && GV->hasLinkOnceODRLinkage() &&
          GV->isDSOLocal() && !GV->hasDLLExportStorageClass());
}

static bool scopedCalls(const GlobalVariable *GV,
                        SmallVectorImpl<CallInst *> &Calls) {
  if (!GV->hasGlobalUnnamedAddr() || !scopedStorage(GV) || GV->hasSection() || GV->isThreadLocal() ||
      GV->getAddressSpace() != 0 || GV->isExternallyInitialized())
    return false;
  auto *Data = dyn_cast<ConstantDataArray>(GV->getInitializer());
  if (!Data || !Data->isString() || Data->getRawDataValues().size() > 4096)
    return false;
  SmallVector<const User *, 16> Worklist(GV->users());
  SmallPtrSet<const User *, 16> Seen;
  SmallPtrSet<CallInst *, 8> SeenCalls;
  while (!Worklist.empty()) {
    const User *U = Worklist.pop_back_val();
    if (!Seen.insert(U).second)
      continue;
    if (auto *CE = dyn_cast<ConstantExpr>(U)) {
      if (CE->getOpcode() != Instruction::GetElementPtr &&
          CE->getOpcode() != Instruction::BitCast)
        return false;
      Worklist.append(CE->user_begin(), CE->user_end());
      continue;
    }
    auto *CI = dyn_cast<CallInst>(const_cast<User *>(U));
    if (!CI || CI->isMustTailCall() || !CI->doesNotThrow() ||
        CI->hasFnAttr(Attribute::ReturnsTwice) || CI->hasOperandBundles() || hasExceptionHandling(*CI->getFunction()))
      return false;
    // Examine the actual operand use, not merely the callee declaration:
    // one call can use this literal in more than one argument position.
    for (const Use &Arg : CI->args()) {
      if (!refersToString(Arg.get(), GV))
        continue;
      unsigned Index = CI->getArgOperandNo(&Arg);
      if (!CI->doesNotCapture(Index) || !CI->onlyReadsMemory(Index))
        return false;
    }
    // Global addresses used as a callee are never string consumers.
    if (refersToString(CI->getCalledOperand(), GV))
      return false;
    if (SeenCalls.insert(CI).second)
      Calls.push_back(CI);
  }
  return !Calls.empty();
}

bool hasOnlyScopedStringUses(const GlobalVariable *GV) {
  SmallVector<CallInst *, 8> Calls;
  return scopedCalls(GV, Calls);
}

// Rebuild a constant GEP/cast with an instruction-valued buffer as its base.
static Value *scopedOperand(Value *V, GlobalVariable *GV, Value *Buffer,
                            IRBuilder<> &B) {
  if (V == GV)
    return Buffer;
  auto *CE = dyn_cast<ConstantExpr>(V);
  if (!CE)
    return V;
  SmallVector<Value *, 4> Operands;
  bool Changed = false;
  for (Value *Op : CE->operands()) {
    Value *New = scopedOperand(Op, GV, Buffer, B);
    Operands.push_back(New);
    Changed |= New != Op;
  }
  if (!Changed)
    return V;
  Instruction *I = CE->getAsInstruction();
  for (unsigned N = 0; N < Operands.size(); ++N)
    I->setOperand(N, Operands[N]);
  return B.Insert(I);
}

void emitScopedStringUses(GlobalVariable *GV, Function *Stub) {
  SmallVector<CallInst *, 8> Calls;
  bool Safe = scopedCalls(GV, Calls);
  assert(Safe && "scoped string uses must be validated before mutation");
  (void)Safe;
  auto *Type = cast<ArrayType>(GV->getValueType());
  for (CallInst *CI : Calls) {
    Function &F = *CI->getFunction();
    IRBuilder<> Entry(&*F.getEntryBlock().getFirstInsertionPt());
    auto *Buffer = Entry.CreateAlloca(Type, nullptr, "kagura.scoped");
    if (auto A = GV->getAlign())
      Buffer->setAlignment(*A);
    CI->setTailCallKind(CallInst::TCK_None); // Cleanup must run before returning.
    IRBuilder<> Before(CI);
    Before.CreateCall(Stub, {Buffer});
    for (Use &Arg : CI->args())
      Arg.set(scopedOperand(Arg.get(), GV, Buffer, Before));
    IRBuilder<> After(CI->getNextNode());
    After.CreateMemSet(Buffer, After.getInt8(0), Type->getNumElements(),
                      Buffer->getAlign(), /*isVolatile=*/true);
    invalidateMemoryEffects(F);
  }
}

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
    if (StrictLinkage && !GV.hasLocalLinkage() &&
        !(opt::StringScoped && hasOnlyScopedStringUses(&GV)))
      continue;
    auto *CDA = dyn_cast<ConstantDataArray>(GV.getInitializer());
    if (!CDA || !CDA->isString())
      continue;
    StringRef S = CDA->getAsString();
    if (opt::StringAggressive) {
      if (!S.empty() && ((opt::StringScoped && hasOnlyScopedStringUses(&GV)) ||
                         canProtectStringAtStartup(GV)))
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
