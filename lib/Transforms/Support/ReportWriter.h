//===-- ReportWriter.h - Per-module JSONL report output -------------------===//

#pragma once

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

namespace kagura {

void appendReportRecord(llvm::StringRef Path, llvm::StringRef ReportName,
                        llvm::json::Object Record);

} // namespace kagura
