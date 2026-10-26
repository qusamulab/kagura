//===-- ObfuscationPolicy.cpp - Protection selection ----------------------===//

#include "kagura/Options.h"
#include "kagura/Utils.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"

using namespace llvm;

namespace kagura {

// ---- Annotation helpers ----

bool hasAnnotation(Function &F, StringRef Attr) {
  // AutoSelect writes function metadata. Check it before walking the global
  // annotation table, which contains annotations emitted by clang.
  if (F.hasMetadata(Attr))
    return true;

  Module *M = F.getParent();
  GlobalVariable *GA = M->getGlobalVariable("llvm.global.annotations");
  if (!GA)
    return false;
  auto *CA = dyn_cast<ConstantArray>(GA->getInitializer());
  if (!CA)
    return false;
  for (unsigned I = 0; I < CA->getNumOperands(); ++I) {
    auto *CS = dyn_cast<ConstantStruct>(CA->getOperand(I));
    if (!CS || CS->getNumOperands() < 2)
      continue;
    auto *FPtr = dyn_cast<Function>(CS->getOperand(0)->stripPointerCasts());
    if (FPtr != &F)
      continue;
    auto *GV = dyn_cast<GlobalVariable>(CS->getOperand(1)->stripPointerCasts());
    if (!GV || !GV->hasInitializer())
      continue;
    auto *Str = dyn_cast<ConstantDataArray>(GV->getInitializer());
    if (!Str)
      continue;
    if (Str->getAsCString().contains(Attr))
      return true;
  }
  return false;
}

bool isKaguraSymbol(StringRef Name) {
  // Every prefix any kagura pass emits. Keep in sync when adding a pass that
  // creates new globals or helper functions; a missing entry means later
  // passes re-obfuscate our own helpers and SymbolMap reports them as user
  // symbols.
  static constexpr StringRef Prefixes[] = {
      "kagura_",  // runtime calls, decrypt stubs, keys, flags, honey values
      "__kagura", // CallIndirection thunk table, StringSplit initialisers
      "__kg_",    // FunctionSplit outlined helpers
      "kagura.",  // EncryptedLookupTable and ControlFlowFlattening globals
  };
  for (StringRef P : Prefixes)
    if (Name.starts_with(P))
      return true;
  return false;
}

// ---- Protection filters --------------------------------------------------

/// Returns true if Name matches any pattern in the comma-separated list.
/// Patterns may use a trailing '*' glob (e.g. "my_prefix_*").
static bool matchesList(StringRef Name, StringRef List) {
  if (List.empty())
    return false;
  SmallVector<StringRef, 16> Patterns;
  List.split(Patterns, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  for (StringRef Pat : Patterns) {
    Pat = Pat.trim();
    if (Pat.empty())
      continue;
    if (Pat.ends_with("*")) {
      if (Name.starts_with(Pat.drop_back(1)))
        return true;
    } else {
      if (Name == Pat)
        return true;
    }
  }
  return false;
}

bool shouldObfuscate(Function &F, StringRef PassAttr, bool GlobalFlag) {
  // Keep injected helpers out of later obfuscation passes.
  if (isKaguraSymbol(F.getName()))
    return false;

  // Avoid transforming functions instrumented by these sanitizers.
  if (F.hasFnAttribute(Attribute::SanitizeAddress) ||
      F.hasFnAttribute(Attribute::SanitizeThread) ||
      F.hasFnAttribute(Attribute::SanitizeMemory) ||
      F.hasFnAttribute(Attribute::SanitizeHWAddress))
    return false;

  StringRef Name = F.getName();

  // Explicit protection takes precedence over the filters below.
  if (!opt::ProtectList.empty() && matchesList(Name, opt::ProtectList))
    return true;

  // Exclude matching functions unless force-protected above.
  if (!opt::DenyList.empty() && matchesList(Name, opt::DenyList))
    return false;

  // Skip performance-critical functions.
  if (hasAnnotation(F, "kagura_hotpath"))
    return false;

  // In allowlist mode, only matching symbols are obfuscated.
  if (!opt::AllowList.empty() && !matchesList(Name, opt::AllowList))
    return false;

  std::string EnableAttr = ("kagura_" + PassAttr).str();
  std::string DisableAttr = ("kagura_no" + PassAttr).str();
  if (hasAnnotation(F, DisableAttr))
    return false;
  if (hasAnnotation(F, EnableAttr))
    return true;
  return GlobalFlag;
}

} // namespace kagura
