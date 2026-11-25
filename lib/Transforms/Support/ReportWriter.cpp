//===-- ReportWriter.cpp - Per-module JSONL report output -----------------===//

#include "ReportWriter.h"

#include "llvm/ADT/Twine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <mutex>
#include <system_error>
#include <utility>

using namespace llvm;

namespace kagura {

void appendReportRecord(StringRef Path, StringRef ReportName,
                        json::Object Record) {
  // Threads in the same compiler share a mutex. Separate compiler processes
  // share the file lock. Hold both until the whole record has been flushed.
  static std::mutex OutputMutex;
  std::lock_guard<std::mutex> Guard(OutputMutex);
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_Append | sys::fs::OF_Text);
  if (EC)
    report_fatal_error(Twine("[kagura] cannot open ") + ReportName + " " +
                           Path + ": " + EC.message(), false);
  auto Lock = OS.lock();
  if (!Lock)
    report_fatal_error(Twine("[kagura] cannot lock ") + ReportName + " " +
                           Path + ": " + toString(Lock.takeError()), false);
  OS << json::Value(std::move(Record)) << "\n";
  OS.flush();
  if (OS.has_error())
    report_fatal_error(Twine("[kagura] cannot write ") + ReportName + " " +
                           Path + ": " + OS.error().message(), false);
}

} // namespace kagura
