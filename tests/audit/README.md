# IR transformation safety audit (2026-10-26)

The audit covers all 36 entries in `include/kagura/PassRegistry.def`: 30
obfuscation passes and six infrastructure passes. The source review follows
PHI edges, dominance after CFG changes, EH/token and musttail constraints,
complete pointer-use graphs, access types/address spaces, and storage lifetime.
It is a bounded correctness audit, not a proof that every input is bug-free.

## Reproduction and validation

Local environment: LLVM 21.1.8, macOS arm64. The baseline is the combined #57
and #58 implementation, before these audit fixes. Eleven focused regression
files fail against that baseline and pass after the fixes. Nine new lit files
and six expanded lit files cover the transformation boundaries. Seven native
integration cases cover escaped/offset string pointers, concurrent first use,
and external calls from early constructors.

```sh
cmake --build build_issues -j 8
ctest --test-dir build_issues --output-on-failure -j 8
LLVM_PREFIX=/opt/homebrew/opt/llvm@21 \
  ./scripts/ci/differential-test.sh build_issues/lib/Transforms/KaguraObfuscator.dylib
python3 scripts/ci/ir-safety-audit.py \
  --opt /opt/homebrew/opt/llvm@21/bin/opt \
  --plugin "$PWD/build_issues/lib/Transforms/KaguraObfuscator.dylib" \
  --output build_issues/audit/matrix
```

The cross-pass runner applies every registry entry to all 48 standalone lit
modules with seeds 32 and 42 (3,456 combinations). Each output passes the LLVM
verifier and is printed and reparsed: non-assertions LLVM can otherwise miss
malformed constant expressions. BCF/DCI probabilities are 100%; VTP and DWARF
obfuscation are explicitly enabled. Configuration and reporting infrastructure
also have dedicated policy tests; their default matrix invocation may be a
no-op. A successful matrix invocation does not assert that a pass transformed
every module. Positive FileCheck assertions and native differential execution
are separate checks.

## Coverage and findings

| Pass | Result of review and regression checks |
| --- | --- |
| `ci` | Preserve each call site's prototype; resolve/cache imports on first use so earlier application constructors cannot call an empty slot. Publish cache entries atomically. |
| `pac` | Require complete, correctly typed local access graphs; use LLVM's actual i64 PAC intrinsic signatures; authenticate all loads and sign all stores. Guard hardware initialization before first access as well as in its constructor. |
| `str` | Preserve constant-expression GEPs/offsets by replacing their base; validate all uses before mutation. Synchronize first decryption and invalidate stale function/call memory effects. |
| `str-aes` | Preserve offsets and pointer identity/lifetime with permanent per-literal backing; synchronize lazy decryption. Startup/aggressive mode keeps constructor initialization. |
| `wstr` | Validate the whole use graph before encryption; cover ConstantExpr accesses with the shared guard and decline unsupported PHI/static/EH use graphs in default mode. |
| `string-split` | Validate complete use graphs, preserve constant expressions and offsets, initialize PHI operands in incoming blocks, and synchronize recombination. |
| `tamper` | Reviewed entry insertion and constructor generation; no additional defect reproduced in the audited patterns. |
| `objc` | Reviewed global replacement/constant users and entry injection; platform runtime behavior remains outside native execution coverage. |
| `jni` | Handle an existing OnLoad declaration, decline incompatible hooks before renaming, and emit legal local visibility. Functional registration remains incomplete; see limitations below. |
| `anti-debug` | Reviewed constructor/check-block construction and hook reuse; no additional defect reproduced in the audited patterns. |
| `fsplit` | Reject token operands, operand bundles and returns_twice calls before moving instructions across function frames. Existing live-out/PHI tests retained. |
| `genc` | Validate every use before encryption, including constant offsets and exact load width. Decline escaped, dynamic-index, mixed-type, volatile/atomic or special storage; remove stale plaintext range metadata. |
| `honey` | Reviewed generated constructors and globals; no additional defect reproduced in the audited patterns. |
| `sv` | Reviewed linkage/visibility changes; no additional defect reproduced in the audited patterns. |
| `fla` | Spill non-entry alloca pointer results across dispatcher edges; reject EH before mutation. |
| `bcf` | Reviewed PHI-aware splitting and junk generation; cross-tested with probability 100%. No additional defect reproduced in the audited patterns. |
| `sub` | Reviewed integer width/type eligibility and replacement uses; no additional defect reproduced in the audited patterns. |
| `cse-break` | Reviewed insertion/dominance and integer eligibility; no additional defect reproduced in the audited patterns. |
| `co` | Reviewed operand rewriting/PHI treatment and integer widths; no additional defect reproduced in the audited patterns. |
| `vm` | Decline volatile memory, repeated non-entry allocations, block-address uses, unsupported pointer layouts, bundled/convergent/returns_twice calls. Narrow wide constant GEP indices using the supported pointer index width. |
| `ibr` | Reviewed call-site types and block-address branching; no additional defect reproduced in the audited patterns. |
| `lt` | Reviewed latch/PHI rewrites and dominance; retained latch-defined-step negative regression. No additional defect reproduced in the audited patterns. |
| `bbr` | Reviewed entry/EH block preservation and analysis invalidation; no additional defect reproduced in the audited patterns. |
| `dci` | Reviewed dead-block insertion and analysis invalidation; cross-tested with probability 100%. No additional defect reproduced in the audited patterns. |
| `bbs` | Keep musttail calls adjacent to their optional return bitcast when selecting split points. |
| `mvo` | Require every access to match the allocated integer type; decline volatile/atomic accesses and remove plaintext range metadata from ciphertext loads. |
| `pe` | Require matching pointer accesses; preserve slot address space/alignment, use the stored pointer's width and decline non-integral or volatile/atomic accesses. |
| `telemetry` | Reviewed entry insertion and declaration reuse; no additional defect reproduced in the audited patterns. |
| `bbcheck` | Reviewed PHI/EH insertion and split successor updates; no additional defect reproduced in the audited patterns. |
| `elt` | Use the function's actual return type; bounds-check wide conditions before narrowing; avoid signed/64-bit extraction assumptions for cases/results and support unsigned byte outputs. |
| `autoselect` | Reviewed annotation/metadata-only updates; covered by policy/selection tests. |
| `config` | Reviewed pipeline construction and option precedence; covered by strict/precedence/profile policy tests. |
| `dwarf-control` | Preserve the distinct subprogram node, compile unit, lexical scopes and instruction references when changing its file. Exercise obfuscate mode with the verifier. |
| `symmap` | Reviewed read-only traversal/output; no additional IR/reference defect found in the audited patterns. |
| `vtp` | Reject unknown MSVC descriptor layouts before rebuilding their initializer; retain normal three-field descriptor coverage. |
| `audit` | Reviewed metadata traversal/output; no additional IR/reference defect found in the audited patterns. |

Shared EH recognition now includes `catchswitch`. The common lazy initializer
uses a 0/uninitialized, 1/initializing, 2/ready protocol with cmpxchg and
acquire/release publication. Default AES plaintext now persists for the literal's
lifetime; this preserves application pointers but does not provide stack-only
plaintext lifetime. Conservative skips can reduce protection coverage, while
retaining the original program's behavior.

## Limits and remaining work

- The local run validates LLVM 21 on macOS arm64. Other supported LLVM versions,
  Windows/Linux/Android builds and assertions-enabled builds require CI.
- Windows funclet IR and ARM64e PAC signatures are verified as IR. This is not
  device execution or validation of runtime/linker startup ordering on iOS.
- The JNI pass still has broader pre-existing functionality gaps: its runtime
  helper implementations are absent, signatures are empty, and registration
  does not pass the native function address. This audit repairs unsafe IR/hook
  mutation only; it does **not** make dynamic JNI registration operational.
- Objective-C, CFString/Swift/Kotlin special sections and runtime loader behavior
  need platform/device integration tests. The matrix verifies their IR shape;
  it does not establish successful platform initialization or writable placement.
- The corpus and fixed seeds cover specific transformation boundaries. Native
  comparisons and concurrent tests strengthen that coverage, but do not prove
  equivalence for arbitrary input, all pass orders, or all thread schedules.
