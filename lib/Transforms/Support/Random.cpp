//===-- Random.cpp - Deterministic randomness and hashing -----------------===//

#include "kagura/Options.h"
#include "kagura/Utils.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cassert>

using namespace llvm;

namespace kagura {

// ---- Hashing --------------------------------------------------------------

// FNV-1a parameters. Mirrored in runtime/core/hash.c — the runtime recomputes
// hashes that these produce at compile time, so the two must agree exactly.
static constexpr uint32_t kFnv1a32Offset = 0x811c9dc5u;
static constexpr uint32_t kFnv1a32Prime = 0x01000193u;
static constexpr uint64_t kFnv1a64Offset = 0xcbf29ce484222325ULL;
static constexpr uint64_t kFnv1a64Prime = 0x100000001b3ULL;

uint32_t fnv1a32(ArrayRef<uint8_t> Data) {
  uint32_t H = kFnv1a32Offset;
  for (uint8_t B : Data) {
    H ^= B;
    H *= kFnv1a32Prime;
  }
  return H;
}

uint32_t fnv1a32(StringRef S) {
  return fnv1a32(
      ArrayRef<uint8_t>(reinterpret_cast<const uint8_t *>(S.data()), S.size()));
}

uint64_t fnv1a64(StringRef S) {
  uint64_t H = kFnv1a64Offset;
  for (char C : S) {
    H ^= static_cast<uint8_t>(C);
    H *= kFnv1a64Prime;
  }
  return H;
}

// ---- PRNG ----

// Fixed default seed used when neither -kagura-seed nor -kagura-build-id is
// given. Chosen so the tool is reproducible by default (same IR in -> same
// binary out), which is what CI and supply-chain verification want. Callers
// who need a unique key per build opt in with -kagura-build-id=<git-sha> or an
// explicit -kagura-seed; both feed through getModulePRNG() below.
static constexpr uint64_t kDefaultSeed = 0x6b6167757261ULL; // "kagura"

PRNG::PRNG(uint64_t Seed) {
  // A zero seed is treated as "use the deterministic default" rather than
  // reaching for wall-clock / std::random_device entropy: obfuscation keys are
  // embedded in (and recoverable from) the output binary regardless, so a
  // random default buys no real secrecy while destroying reproducibility.
  State = Seed ? Seed : kDefaultSeed;
}

uint64_t PRNG::next() {
  uint64_t Z = (State += 0x9e3779b97f4a7c15ULL);
  Z = (Z ^ (Z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  Z = (Z ^ (Z >> 27)) * 0x94d049bb133111ebULL;
  return Z ^ (Z >> 31);
}

uint32_t PRNG::next32() { return static_cast<uint32_t>(next()); }

uint64_t PRNG::nextRange(uint64_t Lo, uint64_t Hi) {
  assert(Hi > Lo);
  return Lo + (next() % (Hi - Lo));
}

/// The -kagura-seed / -kagura-build-id pair, folded into one value.
static uint64_t baseSeed() {
  uint64_t Seed = opt::Seed;
  // 4.2.7: Mix BuildID into seed for per-build key rotation.
  // Using FNV-1a over the BuildID string ensures unique keys even when
  // the user specifies a fixed -kagura-seed for reproducible output.
  if (!opt::BuildID.empty())
    Seed ^= fnv1a64(opt::BuildID.getValue());
  return Seed;
}

PRNG &getModulePRNG(const Module &M) {
  // Cache the current module's stream per thread so parallel ThinLTO backends
  // do not race. Seed from the basename to keep output independent of the
  // checkout path; verify-reproducible.sh relies on that property.
  //
  // Two files with the same basename in different directories share a stream.
  // Use -kagura-build-id to differentiate builds when needed.
  struct Cache {
    std::string Id;
    PRNG Gen{0};
    bool Valid = false;
  };
  static thread_local Cache C;

  StringRef Id = sys::path::filename(M.getModuleIdentifier());
  if (!C.Valid || C.Id != Id) {
    C.Id = Id.str();
    C.Gen = PRNG(baseSeed() ^ fnv1a64(Id));
    C.Valid = true;
  }
  return C.Gen;
}

uint64_t randomForWidth(PRNG &RNG, unsigned Bits) {
  // next32() below 32 bits keeps the draw count identical to what the passes
  // that already masked by hand were doing, so this does not perturb the key
  // stream of anything that was already correct.
  uint64_t Raw = Bits <= 32 ? RNG.next32() : RNG.next();
  return Raw & maskForWidth(Bits);
}

void fillRandomBytes(PRNG &RNG, uint8_t *Out, size_t Len) {
  for (size_t I = 0; I < Len; I += 8) {
    uint64_t V = RNG.next();
    size_t N = std::min(static_cast<size_t>(8), Len - I);
    for (size_t J = 0; J < N; ++J)
      Out[I + J] = static_cast<uint8_t>((V >> (J * 8)) & 0xFF);
  }
}

} // namespace kagura
