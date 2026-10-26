# Architecture

```
kagura/
├── include/kagura/
│   ├── PassRegistry.def    The pass list. Generates the flags, the pipeline,
│   │                       the JSON policy keys and the link smoke tests
│   ├── Passes/             One header per lib/Transforms/ subdirectory
│   ├── Passes.h            Umbrella over Passes/; for Plugin.cpp and the fuzzers
│   ├── Options.h           CLI flag declarations (generated from the registry)
│   ├── Utils.h             Shared IR helpers, PRNG, target-triple predicates
│   ├── VM.h + VMOpcodes.def  The bytecode contract, shared with the C runtime
│   └── game_protect.h      Protected<T> — the one consumer-facing header
├── lib/Transforms/
│   ├── ABI/                C++ RTTI names and vtable integrity
│   ├── AntiAnalysis/       Anti-debug, integrity, call indirection, honey values
│   ├── CFG/                Control-flow obfuscation passes
│   ├── Data/               String / constant / global / wide-string / memory-value encryption
│   ├── Infrastructure/     Policy, metrics, symbol map, audit log
│   ├── Platform/           iOS (ObjC), Android (JNI)
│   ├── VM/                 Function virtualization
│   ├── Support/            Shared helper implementations and private headers
│   ├── Profiles.def        FAST / BALANCED / STRONG. Generates integration/profiles/*.json
│   ├── Options.cpp         CLI flag definitions
│   └── Plugin.cpp          Pass registration & pipeline wiring
├── runtime/
│   ├── core/               AES, VM interpreter, crash symbolication, device key
│   ├── anti_debug/         Anti-debug / anti-Frida (cross-platform POSIX)
│   ├── android/            Android + Linux: root detection, attestation, /proc, syscall
│   ├── ios/                iOS / macOS: jailbreak detection, Mach-O integrity
│   ├── windows/            Windows: IsDebuggerPresent, NtQueryInformationProcess, PE integrity
│   └── game/               Anti-cheat, IL2CPP protection, telemetry
├── integration/            Xcode, Gradle, Unity, Unreal, CMake, Bazel, CocoaPods, SPM
├── scripts/
│   ├── cli/                Tools you run on your own build (config, strip, diff, variants)
│   ├── eval/               Cost model, battery estimate, benchmarks
│   └── ci/                 Differential test, reproducible-build check, profile generator
└── tests/                  CTest + FileCheck lit-based regression tests
```

## Shared pass support

`include/kagura/Utils.h` declares the shared helper API. Its implementations
live under `lib/Transforms/Support/`, grouped by responsibility:

| File | Responsibility |
|:-----|:---------------|
| `ObfuscationPolicy.cpp` | Function annotations, generated symbol names and protection filters |
| `Random.cpp` | Deterministic PRNG, seed derivation, hashing and random bytes |
| `Target.cpp` | LLVM target-triple compatibility and architecture checks |
| `IRUtils.cpp` | Generated IR markers, integer widths, constructors, EH checks and IR rewriting |
| `StringUtils.cpp` | String selection, use analysis and synchronized decryption guards |
| `AES128.h` | Private AES implementation shared by encryption passes |

CMake discovers support sources automatically and compiles them individually,
including in unity builds. Each file includes its own dependencies. Pass
implementations belong in the category directories; shared helpers belong here.

## Plugin entry point

`lib/Transforms/Plugin.cpp` registers all passes with the LLVM **New Pass
Manager** via `PassPluginLibraryInfo`. It does two things:

1. Exposes every pass by name so it can be requested from `opt`
   (`-passes="kagura-str,..."`) or from clang via `-mllvm -kagura-<name>`.
2. Auto-wires the [recommended order](pass-order.md) onto the
   `OptimizerLast` extension point so users only need
   `-fpass-plugin=KaguraObfuscator.dylib` to get a sensible default pipeline.

## Configuration & options

Per-pass enable flags and numeric tuning parameters are generated from
`PassRegistry.def` — both the definitions in `Options.cpp` and the extern
declarations in `Options.h` — so neither can drift from the pass list. Adding a
tunable means adding a `KAGURA_TUNING` row, then reading the flag from the pass.
Only the string-valued and pipeline-control flags, which have no per-pass row,
are written by hand.

The [`kagura-config`](configuration.md) loader is **not** a pass. It is called
from `Plugin.cpp` before the pipeline is constructed, because the `opt::`
values are read at construction time to decide which passes to add — a loader
running as a pipeline pass could never affect that decision.
