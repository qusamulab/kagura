#include "kagura/Options.h"
#include "kagura/Utils.h"
#include "llvm/IR/LLVMContext.h"
#include <cstdlib>

static void check(bool OK) {
  if (!OK)
    std::abort();
}

int main() {
  llvm::LLVMContext C;
  llvm::Module A("/one/common.c", C), B("/two/common.c", C),
      Other("other.c", C);
  // Interleaving same-basename modules must never advance another's stream.
  kagura::opt::Seed = 32;
  auto Expected = kagura::getModulePRNG(B, "StringEncryption");
  auto First = Expected.next();
  auto Noise = kagura::getModulePRNG(A, "StringEncryption");
  for (unsigned I = 0; I < 100; ++I)
    Noise.next();
  auto Actual = kagura::getModulePRNG(B, "StringEncryption");
  check(Actual.next() == First);
  auto Intervening = kagura::getModulePRNG(Other, "StringEncryption");
  Intervening.next();
  check(Actual.next() == Expected.next());
  check(kagura::getModulePRNG(B, "Substitution:f").next() != First);
  check(kagura::getModulePRNG(B, "Substitution:f").next() !=
        kagura::getModulePRNG(B, "Substitution:g").next());
  A.setSourceFileName("one/common.c");
  B.setSourceFileName("two/common.c");
  check(kagura::getModulePRNG(A).next() != kagura::getModulePRNG(B).next());
  auto Before = kagura::getModulePRNG(B).next();
  kagura::opt::Seed = 33;
  check(kagura::getModulePRNG(B).next() != Before);
  kagura::opt::Seed = 32;
  check(kagura::getModulePRNG(B).next() == Before);
  kagura::opt::BuildID = "next-build";
  check(kagura::getModulePRNG(B).next() != Before);
}
