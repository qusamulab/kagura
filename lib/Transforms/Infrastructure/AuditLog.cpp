//===-- AuditLog.cpp - Obfuscation audit log emission --------------------===//
//
// 4.6.10: Emits a JSON audit log that records what was protected and how.
//
// The log is appended (one JSON object per compilation unit) to the output
// file specified by -kagura-audit-out (default: kagura_audit.json).
//
// Each record contains:
//   "module"   : source file name (from !llvm.ident or module name)
//   "timestamp": seconds since epoch
//   "build_id" : value of -kagura-build-id if set
//   "functions": array of { "name": "...", "passes": ["fla","bcf",...] }
//
// Registry adapters observe changed IR and referenced storage when auditing is
// enabled. Existing pass-specific markObfuscated() calls remain compatible.
// "pass_results" includes unchanged invocations, with the limitations in notes.
//
// Pass key:   "kagura-audit"
// CLI flags:  -kagura-audit  -kagura-audit-out=<path>
//
//===----------------------------------------------------------------------===//

#include "kagura/Options.h"
#include "kagura/Passes/Infrastructure.h"
#include "kagura/Utils.h"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/ErrorHandling.h"

#include <chrono>
#include <system_error>
#include <mutex>

using namespace llvm;

namespace kagura {

// ---- Metadata helpers -------------------------------------------------------

/// Attach a kagura_obfuscated metadata node to F recording the pass name.
/// Multiple calls accumulate pass names in a single MDTuple.
void markObfuscated(Function &F, StringRef PassName) {
  LLVMContext &Ctx = F.getContext();
  MDNode *Existing = F.getMetadata("kagura_obfuscated");
  SmallVector<Metadata *, 8> Ops;
  if (Existing)
    for (const MDOperand &Op : Existing->operands()) {
      if (cast<MDString>(Op.get())->getString() == PassName)
        return;
      Ops.push_back(Op.get());
    }
  Ops.push_back(MDString::get(Ctx, PassName));
  F.setMetadata("kagura_obfuscated", MDNode::get(Ctx, Ops));
}

/// Returns all pass names recorded on F via markObfuscated().
static SmallVector<std::string, 8> getObfuscatedPasses(const Function &F) {
  SmallVector<std::string, 8> Passes;
  MDNode *MD = F.getMetadata("kagura_obfuscated");
  if (!MD)
    return Passes;
  for (const MDOperand &Op : MD->operands())
    if (auto *MDS = dyn_cast<MDString>(Op.get()))
      Passes.push_back(MDS->getString().str());
  return Passes;
}

// ---- Pass ------------------------------------------------------------------

PreservedAnalyses AuditLogPass::run(Module &M, ModuleAnalysisManager &) {
  if (!opt::AuditLog)
    return PreservedAnalyses::all();

  // Collect functions that were obfuscated
  json::Array FunctionsArr;
  for (const Function &F : M) {
    auto Passes = getObfuscatedPasses(F);
    if (Passes.empty())
      continue;
    json::Array PassesArr;
    for (const std::string &P : Passes)
      PassesArr.push_back(P);
    FunctionsArr.push_back(json::Object{
        {"name", F.getName().str()},
        {"passes", std::move(PassesArr)},
    });
  }

  json::Array Results;
  if (const auto *MD = M.getNamedMetadata("kagura.audit.results"))
    for (const auto *Node : MD->operands())
      Results.push_back(json::Object{
          {"pass", cast<MDString>(Node->getOperand(0))->getString().str()},
          {"scope", cast<MDString>(Node->getOperand(1))->getString().str()},
          {"name", cast<MDString>(Node->getOperand(2))->getString().str()},
          {"status", cast<MDString>(Node->getOperand(3))->getString().str()}});

  // Build record
  auto Now = std::chrono::system_clock::now().time_since_epoch();
  int64_t Timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(Now).count();

  std::string ModuleName = M.getName().str();
  if (ModuleName.empty())
    ModuleName = "<unknown>";

  json::Object Record{
      {"module",     ModuleName},
      {"timestamp",  Timestamp},
      {"build_id",   opt::BuildID.empty() ? "" : opt::BuildID.getValue()},
      {"functions",  std::move(FunctionsArr)},
      {"pass_results", std::move(Results)},
      {"notes", json::Array{
          "unchanged means excluded, unsupported, or no eligible transformation; "
          "a missing result means the pass was not observed running. "
          "Changed IR is not evidence of protection strength."}},
  };

  // Append to output file
  std::string OutPath = opt::AuditLogOut.empty()
                            ? "kagura_audit.json"
                            : opt::AuditLogOut.getValue();

  std::error_code EC;
  static std::mutex OutputMutex;
  std::lock_guard<std::mutex> Guard(OutputMutex);
  raw_fd_ostream OS(OutPath, EC, sys::fs::OF_Append | sys::fs::OF_Text);
  if (EC)
    report_fatal_error(Twine("[kagura] cannot open audit log: ") + EC.message(), false);
  auto Lock = OS.lock();
  if (!Lock)
    report_fatal_error(Twine("[kagura] cannot lock audit log: ") +
                       toString(Lock.takeError()), false);
  OS << json::Value(std::move(Record)) << "\n";
  OS.flush(); // Keep the lock until the entire JSONL record reaches the file.
  if (OS.has_error())
    report_fatal_error("[kagura] cannot write audit log", false);

  return PreservedAnalyses::all();
}

} // namespace kagura
