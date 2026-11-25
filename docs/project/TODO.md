# TODO — Remaining Work

Only unfinished work belongs here; remove completed items instead of keeping struck-through rows. Status reflects the current working tree, including changes not yet committed or released.

## Additional implementation gaps

These are missing runtime behavior or inaccurate capability claims, independent of resistance scoring or evaluation automation.

| ID | Feature | Priority | Evidence and remaining work |
|:---|:--------|:---------|:----------------------------|
| D.6 | Real code-integrity verification for AntiTamper / BBChecksum | High | `runtime/core/self_check.c::kagura_runtime_hash_check` checks a few entry-instruction patterns and suspicious symbol names; it does not compare a function's machine-code hash, and the IR hash is only mixed into an unused volatile tag. `runtime/core/bb_check.c` always returns intact. Design trusted post-codegen byte ranges, expected hashes and stable identifiers, accounting for relocations/linker transforms, then verify actual code and exercise both clean and patched binaries. |
| D.7 | VTableProtection runtime enforcement | Medium | `ABI/VTableProtection.cpp::recordVTableMetadata` records names and entry counts in IR metadata. Implement runtime verification for supported ABI layouts, preserving virtual dispatch and RTTI behavior. |
| D.8 | Windows ETW detection | Low | The Windows ETW entry points are stubs. Provider enumeration and verified provider identifiers are still needed. |

## Future candidates

| ID | Feature | Priority | Remaining work |
|:---|:--------|:---------|:---------------|
| — | Regression corpus expansion | Medium | The corpus has 8 cases covering EH, PHI-heavy, vararg, empty and large-function shapes. Add dedicated cases for passes that still lack them. |
| — | Advanced protection-target inference | Low | `AutoSelect.cpp` already scores cyclomatic complexity, instruction count, string references, allocas and external calls. Remaining extensions are symbol-name / call-graph features and optionally ML-based recommendations; do not reimplement the existing heuristic selection. |
| — | Machine code / backend-level obfuscation | Low | Add assembler / MachineFunction-level transforms after instruction selection. Coordinate any code-range metadata work with D.6. |
| — | White-box cryptography | Low | Investigate white-box AES/TDES key encoding and its security limits against memory disclosure and key extraction. |
| — | Device farm tests (iOS/Android real devices) | Low | Run representative integration tests on physical devices through a cloud device farm. |
| B.3 | End-to-end deterministic builds | Low | `scripts/ci/verify-reproducible.sh` already checks a subset of passes at the IR level. Extend coverage and assert byte-identical linked binaries for fixed inputs, seed and toolchain, accounting for paths and linker metadata. |
| C.2 | Automated MBA expression generation | Low | Replace or extend fixed substitution patterns with automatically generated, Z3-verified equivalent expressions. |

## Out of current scope

Resistance-evaluation CI integration (D.2) and decompiler-score feedback into AutoSelect (B.1) are deferred by project choice. Existing redteam, symbolic-execution, Ghidra and Frida tools remain available for manual use.
