//===-- Target.cpp - Target compatibility ---------------------------------===//

#include "kagura/Utils.h"

#if __has_include("llvm/TargetParser/Triple.h")
#include "llvm/TargetParser/Triple.h"
#else
#include "llvm/ADT/Triple.h"
#endif

using namespace llvm;

namespace kagura {

// ---- Target triple helpers ----

// Overload set: converts whichever type getTargetTriple() returns to
// std::string. LLVM 17-19 returns const std::string &; LLVM 20+ returns const
// Triple &.
static std::string tripleToString(const std::string &S) { return S; }
static std::string tripleToString(const llvm::Triple &T) { return T.str(); }

std::string getModuleTriple(const Module &M) {
  return tripleToString(M.getTargetTriple());
}

TargetArch getTargetArch(const Module &M) {
  std::string TripleStr = getModuleTriple(M);
  StringRef Triple(TripleStr);
  // arm64e must be checked before generic aarch64 since it is a substring
  // match.
  if (Triple.contains("arm64e"))
    return TargetArch::ARM64e;
  if (Triple.starts_with("aarch64") || Triple.starts_with("arm64"))
    return TargetArch::ARM64;
  if (Triple.starts_with("armv7") || Triple.starts_with("thumbv7"))
    return TargetArch::ARMv7;
  if (Triple.starts_with("x86_64") || Triple.starts_with("amd64"))
    return TargetArch::X86_64;
  if (Triple.starts_with("wasm64"))
    return TargetArch::Wasm64;
  if (Triple.starts_with("wasm32"))
    return TargetArch::Wasm32;
  return TargetArch::Other;
}

bool isAArch64Target(const Module &M) {
  TargetArch A = getTargetArch(M);
  return A == TargetArch::ARM64 || A == TargetArch::ARM64e;
}

bool isArm64eTarget(const Module &M) {
  return getTargetArch(M) == TargetArch::ARM64e;
}

bool isARMv7Target(const Module &M) {
  return getTargetArch(M) == TargetArch::ARMv7;
}

bool isX86_64Target(const Module &M) {
  return getTargetArch(M) == TargetArch::X86_64;
}

bool isWasmTarget(const Module &M) {
  TargetArch A = getTargetArch(M);
  return A == TargetArch::Wasm32 || A == TargetArch::Wasm64;
}

} // namespace kagura
