# Kagura development guide

## Start here

- [Contributor guide](docs/project/CONTRIBUTING.md): build, pass registration,
  runtime conventions and tests.
- [Architecture](docs/architecture.md): source ownership and shared helpers.
- [Testing](docs/testing.md): CTest, lit, integration and regression coverage.
- [Remaining work](docs/project/TODO.md): unfinished work and deferred scope.
- [Changelog](docs/project/CHANGELOG.md): user-visible changes and known issues.
- [MCP development](mcp/README.md): policy authoring, inspection tools and tests.

Development and maintenance documents belong in `docs/project/`. Keep this
file, `README.md` and `LICENSE` at the repository root for discovery. Do not
duplicate the contributor guide here; link to it and keep task-specific rules
close to their implementation.

## Repository map

- `include/kagura/`: pass APIs, shared contracts and registry definitions.
- `lib/Transforms/`: LLVM passes grouped by category; shared code in `Support/`.
- `runtime/`: portable C helpers and explicit platform-specific source groups.
- `tools/`: standalone IR processing and compiler-driver entry points.
- `mcp/`: Python package for read-only MCP tools and shared policy validation.
- `scripts/`: consumer CLI tools, CI helpers and optional evaluation tools.
- `integration/`: build-system adapters and generated strength profiles.
- `tests/`: CTest, lit, integration subjects, regression corpus and fuzz targets.
- `docs/`: published documentation; `docs/project/` holds project maintenance.

## Skills and tools

There is no repository-specific skill package bundled here. When a task needs
an installed skill, read its `SKILL.md` and apply it within the requested
scope. Record reusable Kagura development knowledge in this guide or the
linked project documents, rather than machine-specific absolute paths.

For LLVM/runtime work, use the matching LLVM CLI, CMake, CTest and lit.
Platform UI or app-building skills do not replace compiler/runtime tests.
For MCP work, prefer the shared Python policy/inspection modules and stdio
client tests; do not require a graphical MCP client for routine validation.
Use document/image/presentation skills only for tasks that produce those
artifacts. Keep evaluation and resistance-scoring work out of unrelated fixes.

## Build and verification

Supported upstream LLVM majors are 17–22. Use one matching LLVM installation
for headers, clang, opt, FileCheck and the plugin. Apple clang version numbers
do not establish compatibility. Set `LLVM_PREFIX` explicitly when the system
or Homebrew default is outside the supported range.

```sh
export LLVM_PREFIX=/path/to/matching/llvm
cmake -S . -B build \
  -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm" \
  -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
  -DKAGURA_REQUIRE_LIT=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Install `lit` into your development environment if required. Static-plugin
builds use `-DKAGURA_FORCE_STATIC_PLUGIN=ON` and exercise `kagura-opt` instead
of loading a plugin. Check both paths when changing tool or plugin wiring.

For Python/MCP changes:

```sh
python3 scripts/ci/gen-profiles.py --check
uv run --project mcp python -m unittest discover -s mcp/tests -v
```

For documentation changes, install `docs/requirements.txt` in a development
environment and run `mkdocs build --strict`. Update navigation, snippet paths,
workflow path filters and inbound links when moving documents.

Choose checks for the behavior being changed. IR transformations need a
meaningful lit assertion and, when runtime semantics change, execution/link
coverage. Ensure a new FileCheck test cannot pass against untransformed input.
Use the existing differential/reproducibility scripts for pass-pipeline changes.
Do not treat successful linking or a scaffold runtime as evidence of protection.

## Implementation rules

- `include/kagura/PassRegistry.def` owns pass names, bool flags and numeric
  tuning. `lib/Transforms/Profiles.def` owns strength presets. Regenerate with
  `python3 scripts/ci/gen-profiles.py`; do not edit generated profiles/catalogs.
- Use category headers and existing `Utils.h` helpers. Support sources compile
  independently, including in unity builds, so include their own dependencies.
- Use `shouldObfuscate` for function selection and `isKaguraSymbol` /
  `markGenerated` for generated code. Local IR names may be discarded.
- Preserve calling conventions, attributes, operand bundles, PHIs, exception
  edges, pointer lifetimes and thread semantics. Skip unsupported cases before
  mutation, and report preserved analyses honestly.
- Draw randomness from the module PRNG and honor the seed/build identifier.
  Use target compatibility helpers for LLVM API and platform differences.
- Every cross-translation-unit runtime symbol belongs in `runtime/internal.h`.
  Add files to the appropriate platform source list; do not make unavailable
  platform probes look like successful checks.
- Keep MCP tools read-only and workspace-scoped. Share policy logic with the
  Python CLI, preserve structured results, and keep stdout for stdio transport.
- Remove finished TODO entries and update user-visible behavior in the
  changelog. Describe real capability limits in docs and PRs.

## Working tree and PRs

Inspect the working tree before editing. Preserve unrelated edits and use a
separate checkout when changes overlap other work. Keep documentation moves
and behavioral implementation in separate commits for review. Do not merge or
publish releases unless requested. PR descriptions should state the problem,
resulting behavior, relevant validation and any untested platform limitations.
