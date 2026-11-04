# Compiler Driver

`kagura` compiles C, C++, Objective-C and LLVM IR with upstream LLVM 17–22.
Default builds and new release bundles include it alongside `kagura-opt`.
It requires Python 3.9+ and a matching upstream clang; Apple clang is rejected.
On Windows, run `bin\kagura.cmd` with Python on PATH. The driver uses the
statically linked `kagura-opt` when LLVM does not support loadable plugins.

```bash
export LLVM_PREFIX=/path/to/llvm
build/bin/kagura -O1 -kagura-str -kagura-fla main.c -o app
build/bin/kagura -kagura-config=policy.json main.cpp -o app
build/bin/kagura --config-check policy.json
```

The pipeline is clang frontend → unoptimized bitcode → `kagura-opt` default
optimization and selected passes → clang code generation → optional link.
Clang's LLVM passes are disabled in the first and last stages so the chosen
optimization pipeline runs once. The default level is `-O2`; use `-O0`, `-O1`,
`-O2`, `-O3`, `-Os` or `-Oz` to change it.

## Tool and runtime selection

`--clang`, `--kagura-opt`, `--plugin` and `--runtime` take explicit paths.
`KAGURA_CLANG`, `KAGURA_OPT`, `KAGURA_RUNTIME` and `LLVM_PREFIX` provide defaults.
Without overrides, the driver finds kagura-opt beside itself, its plugin in
the build/install tree, and clang on PATH. It checks clang/kagura-opt major
versions before compiling. `--verbose` prints subprocess argument arrays to
stderr, and failures preserve the failing tool's exit status.

For native linking, the runtime archive is discovered in the build/install
tree and its platform system libraries are added. Both one-step builds and
separate object linking use this discovery:

```bash
build/bin/kagura -kagura-str-aes -c main.c -o main.o
build/bin/kagura main.o -o app
```

Cross-target compilation needs a matching SDK/sysroot and target runtime.
Supplying `-target`, `--target`, `-arch` or a sysroot disables host-runtime
auto-discovery. Pass `--runtime=/path/to/target/runtime.a` and any system
libraries needed by that target. For C++ object-only linking, select clang++
with `--clang`; object files do not identify the source language to this driver.
On Windows, match `-fms-runtime-lib` to the CRT used to build the runtime archive
(CMake's MSVC default is `dll`). Use clang's GNU-style flags, not clang-cl flags.

## Supported modes and boundaries

Multiple source files, source/object/library mixtures, `-c`, `-S`, and
`-emit-llvm` with `-c` or `-S` are supported. Include paths, defines, dependency
files, target flags and linker arguments are forwarded to the appropriate
stages. Preprocessor-only and version-query modes delegate directly to clang.

This is a small driver for explicit compilation commands. Response files,
LTO, `-fpass-plugin`, `-save-temps`, multi-architecture invocations and arbitrary
clang-cl option syntax are unsupported. Existing build-system adapters remain
available for toolchain-specific workflows.

## Check a policy without compiling

```bash
build/bin/kagura --config-check policy.json
# Equivalent native entry point:
build/bin/kagura-opt -kagura-config-check -kagura-config=policy.json
# LLVM-independent authoring check:
python3 scripts/cli/kagura-cli.py config-check policy.json
```

The native check uses the compiler's strict schema, including unselected
flavors, types, ranges and unknown keys, and emits no IR. The source driver
runs it before compiling a supplied `-kagura-config`. MCP and the Python CLI
share a separate authoring validator, which also rejects duplicate literal
JSON keys. None of these checks proves target support or protection strength.
