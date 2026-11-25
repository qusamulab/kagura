//===-- VTableProtection.cpp - RTTI names and vtable metadata     ---------===//
//
// Encrypt supported RTTI name fields and install a constructor to decrypt
// them. For array-initialized vtables, record the name and entry count in
// module metadata. Virtual dispatch and vtable entries are unchanged.
//
// Pass key:   "kagura-vtp"    (module pass)
// CLI flag:   -kagura-vtp
//
// ABI support:
//   Itanium ABI (Linux / macOS / Android / iOS): _ZTV*, _ZTS*, _ZTI*
//   MSVC ABI (Windows): ??_7* (vtables), ??_R0* – ??_R4* (RTTI descriptors)
//     For MSVC TypeDescriptors (??_R0), the embedded class name field is
//     XOR-encrypted in-place via a module constructor.
//
//===----------------------------------------------------------------------===//

#include "kagura/Options.h"
#include "kagura/Passes/ABI.h"
#include "kagura/Utils.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <vector>

using namespace llvm;

namespace kagura {

// ---- Helpers ---------------------------------------------------------------

// Itanium ABI (Linux, macOS, Android, iOS)
static bool isVTableGlobal(const GlobalVariable &GV) {
    return GV.getName().starts_with("_ZTV");
}

static bool isRTTINameGlobal(const GlobalVariable &GV) {
    return GV.getName().starts_with("_ZTS");
}

// MSVC ABI (Windows)
// ??_7 <name> @@  — vtable for class <name>
// ??_R0 / ??_R1 / ??_R2 / ??_R3 / ??_R4 — RTTI descriptors
// ??_C@  — string literals (MSVC uses mangled names for string constants)
static bool isMSVCVTableGlobal(const GlobalVariable &GV) {
    StringRef N = GV.getName();
    return N.starts_with("??_7");
}

static bool isMSVCRTTIGlobal(const GlobalVariable &GV) {
    StringRef N = GV.getName();
    // TypeDescriptor (??_R0), BaseClassDescriptor (??_R1), ClassHierarchyDescriptor
    // (??_R2), CompleteObjectLocator (??_R3), BaseClassArray (??_R4)
    return N.starts_with("??_R0") || N.starts_with("??_R1") ||
           N.starts_with("??_R2") || N.starts_with("??_R3") ||
           N.starts_with("??_R4");
}

/// Obfuscate an MSVC TypeDescriptor name field.
/// The TypeDescriptor layout is: { ptr vftable, ptr spare, char name[] }
/// The "name" field holds the decorated class name (e.g. ".?AVFoo@@").
/// We XOR-encrypt it in-place using a module constructor.
static bool obfuscateMSVCTypeName(GlobalVariable *GV, Module &M, PRNG &RNG) {
    if (!GV->hasInitializer()) return false;
    // TypeDescriptor is a struct; locate the name field (last element).
    auto *ST = dyn_cast<ConstantStruct>(GV->getInitializer());
    if (!ST || ST->getNumOperands() != 3)
      return false;

    auto *NameConst = dyn_cast<ConstantDataArray>(ST->getOperand(2));
    if (!NameConst || !NameConst->isString()) return false;

    StringRef S = NameConst->getAsString();
    if (S.size() < 4) return false;

    LLVMContext &Ctx = M.getContext();
    auto *Int8Ty  = Type::getInt8Ty(Ctx);
    auto *Int64Ty = Type::getInt64Ty(Ctx);
    auto *PtrTy   = PointerType::getUnqual(Ctx);

    // Encrypt the name bytes (preserve NUL terminator)
    uint8_t Key = static_cast<uint8_t>(RNG.next32() | 1);
    std::vector<uint8_t> Enc(S.begin(), S.end());
    for (size_t I = 0; I < Enc.size() - 1; ++I)
        Enc[I] ^= Key;

    // Rebuild the struct with the encrypted name
    auto *EncNameInit = buildByteArrayConstant(Ctx, Enc);
    SmallVector<Constant *, 3> Elems;
    for (unsigned I = 0; I < ST->getNumOperands() - 1; ++I)
        Elems.push_back(ST->getOperand(I));
    Elems.push_back(EncNameInit);
    GV->setInitializer(ConstantStruct::get(ST->getType(), Elems));
    GV->setConstant(false);

    // Build a constructor to decrypt in-place
    // The name field is at a fixed offset = sizeof(ptr) * 2
    auto *Ctor = createCtorFunction(M, GV->getName() + ".msvc_rtti_decrypt");
    Ctor->addFnAttr(Attribute::NoInline);
    Ctor->addFnAttr(Attribute::NoUnwind);

    auto *Entry = &Ctor->getEntryBlock();
    auto *Loop  = BasicBlock::Create(Ctx, "loop",  Ctor);
    auto *Exit  = BasicBlock::Create(Ctx, "exit",  Ctor);

    // Offset of name field = 2 * pointer size
    uint64_t NameOffset = 2 * (M.getDataLayout().getPointerSize());
    uint64_t NameLen    = Enc.size() - 1; // exclude NUL

    IRBuilder<> EB(Entry);
    Value *Base = EB.CreateBitCast(GV, PtrTy, "msvc.base");
    Value *NamePtr = EB.CreateGEP(Int8Ty, Base,
                                   ConstantInt::get(Int64Ty, NameOffset),
                                   "msvc.name");
    EB.CreateBr(Loop);

    IRBuilder<> LB(Loop);
    auto *Idx = LB.CreatePHI(Int64Ty, 2, "idx");
    Idx->addIncoming(ConstantInt::get(Int64Ty, 0), Entry);
    auto *Ptr  = LB.CreateGEP(Int8Ty, NamePtr, Idx, "elem");
    auto *Orig = LB.CreateLoad(Int8Ty, Ptr, "byte");
    auto *Dec  = LB.CreateXor(Orig, ConstantInt::get(Int8Ty, Key), "dec");
    LB.CreateStore(Dec, Ptr);
    auto *Next = LB.CreateAdd(Idx, ConstantInt::get(Int64Ty, 1), "next");
    Idx->addIncoming(Next, Loop);
    auto *Done = LB.CreateICmpEQ(Next, ConstantInt::get(Int64Ty, NameLen));
    LB.CreateCondBr(Done, Exit, Loop);

    IRBuilder<>(Exit).CreateRetVoid();
    appendKaguraCtor(M, Ctor, CtorPriority::RTTI);
    return true;
}

// ---- RTTI name obfuscation -------------------------------------------------

/// XOR-encrypt a typeinfo name string and register an in-place decrypt ctor.
static bool obfuscateRTTIName(GlobalVariable *GV, Module &M, PRNG &RNG) {
    if (!GV->hasInitializer()) return false;
    auto *CDA = dyn_cast<ConstantDataArray>(GV->getInitializer());
    if (!CDA || !CDA->isString()) return false;

    StringRef S = CDA->getAsString();
    if (S.size() < 4) return false;

    std::vector<uint8_t> Encrypted(S.begin(), S.end());
    uint8_t Key = static_cast<uint8_t>(RNG.next32() | 1); // odd key, never zero

    for (size_t I = 0; I < Encrypted.size() - 1; ++I) // preserve NUL terminator
        Encrypted[I] ^= Key;

    // Replace with encrypted constant
    LLVMContext &Ctx = M.getContext();
    auto *EncInit = buildByteArrayConstant(Ctx, Encrypted);
    GV->setInitializer(EncInit);
    GV->setConstant(false); // must be writable for in-place decryption

    // Build a constructor that XOR-decrypts the name in-place
    auto *I8PtrTy = PointerType::getUnqual(Ctx);
    auto *I8Ty  = Type::getInt8Ty(Ctx);
    auto *I64Ty = Type::getInt64Ty(Ctx);

    auto *Ctor = createCtorFunction(M, GV->getName() + ".rtti_decrypt");
    Ctor->addFnAttr(Attribute::NoInline);
    Ctor->addFnAttr(Attribute::NoUnwind);

    auto *Entry = &Ctor->getEntryBlock();
    auto *Loop  = BasicBlock::Create(Ctx, "loop",  Ctor);
    auto *Exit  = BasicBlock::Create(Ctx, "exit",  Ctor);

    IRBuilder<> EB(Entry);
    Value *GVPtr  = EB.CreateBitCast(GV, I8PtrTy, "rtti.ptr");
    Value *Len    = ConstantInt::get(I64Ty, (uint64_t)(Encrypted.size() - 1));
    EB.CreateBr(Loop);

    // Loop: idx = 0; while idx < Len: ptr[idx] ^= Key; idx++
    IRBuilder<> LB(Loop);
    auto *Idx = LB.CreatePHI(I64Ty, 2, "rtti.idx");
    Idx->addIncoming(ConstantInt::get(I64Ty, 0), Entry);

    auto *Ptr  = LB.CreateGEP(I8Ty, GVPtr, Idx, "rtti.elem");
    auto *Orig = LB.CreateLoad(I8Ty, Ptr, "rtti.byte");
    auto *Dec  = LB.CreateXor(Orig, ConstantInt::get(I8Ty, Key), "rtti.dec");
    LB.CreateStore(Dec, Ptr);
    auto *Next = LB.CreateAdd(Idx, ConstantInt::get(I64Ty, 1), "rtti.next");
    Idx->addIncoming(Next, Loop);
    auto *Done = LB.CreateICmpEQ(Next, Len, "rtti.done");
    LB.CreateCondBr(Done, Exit, Loop);

    IRBuilder<>(Exit).CreateRetVoid();

    appendKaguraCtor(M, Ctor, CtorPriority::RTTI);
    return true;
}

// ---- Vtable metadata ------------------------------------------------------

static bool recordVTableMetadata(GlobalVariable *GV, Module &M) {
    if (!GV->hasInitializer()) return false;
    auto *Arr = dyn_cast<ConstantArray>(GV->getInitializer());
    if (!Arr) return false;

    LLVMContext &Ctx = M.getContext();
    auto *Int64Ty = Type::getInt64Ty(Ctx);
    NamedMDNode *VTInfo = M.getOrInsertNamedMetadata("kagura.vtables");
    SmallVector<Metadata *, 2> Operands;
    Operands.push_back(MDString::get(Ctx, GV->getName()));
    Operands.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Int64Ty, Arr->getNumOperands())));
    VTInfo->addOperand(MDTuple::get(Ctx, Operands));

    return true;
}

// ---- Pass entry point ------------------------------------------------------

PreservedAnalyses VTableProtectionPass::run(Module &M, ModuleAnalysisManager &) {
    if (!kagura::opt::VTP)
        return PreservedAnalyses::all();

    auto RNG = getModulePRNG(M, "VTableProtection");
    bool Changed = false;

    SmallVector<GlobalVariable *, 32> VTables;
    SmallVector<GlobalVariable *, 32> RTTINames;
    SmallVector<GlobalVariable *, 32> MSVCRTTIs;

    for (auto &GV : M.globals()) {
        // Itanium ABI
        if (isVTableGlobal(GV))    VTables.push_back(&GV);
        if (isRTTINameGlobal(GV))  RTTINames.push_back(&GV);
        // MSVC ABI
        if (isMSVCVTableGlobal(GV))  VTables.push_back(&GV);
        if (isMSVCRTTIGlobal(GV))    MSVCRTTIs.push_back(&GV);
    }

    for (auto *GV : RTTINames)
        if (obfuscateRTTIName(GV, M, RNG))
            Changed = true;

    // MSVC TypeDescriptor name obfuscation (??_R0 = TypeDescriptor has name field)
    for (auto *GV : MSVCRTTIs)
        if (GV->getName().starts_with("??_R0"))
            if (obfuscateMSVCTypeName(GV, M, RNG))
                Changed = true;

    for (auto *GV : VTables)
        if (recordVTableMetadata(GV, M))
            Changed = true;

    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace kagura
