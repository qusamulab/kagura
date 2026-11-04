# Kagura paper and reproduction

`paper.tex` is the canonical manuscript; `refs.bib` is its bibliography.
It combines a selected-pass study with a separate evaluation of the profiles
shipped at the recorded source revision. Both experiments use LLVM 21.1.8
on arm64 macOS.

The two experiments use different backend optimization settings. Their
baselines and ratios must not be compared as if only pass selection changed.
The paper explicitly describes both protocols.

## Build the PDF

From `paper/`:

```sh
python3 render_results.py
pdflatex -interaction=nonstopmode -halt-on-error paper.tex
bibtex paper
pdflatex -interaction=nonstopmode -halt-on-error paper.tex
pdflatex -interaction=nonstopmode -halt-on-error paper.tex
```

The source uses the local IEEEtran class/style and common LaTeX packages.
It also compiles in the Codex desktop LaTeX editor. Generated PDF
and auxiliary files are ignored by the repository's existing `paper/*` rules.

## Current-profile experiment

The recorded experiment uses source revision
`cbab339d5486377e3c4fb722017bc72c854c4df2`, LLVM 21.1.8, Apple M4 Pro,
and macOS 26.7.1. The toolkit itself was not changed for the experiment.
The new evaluation sources are identified by their hashes in the result file.
The measurements are historical: they have not been relabeled as measurements
of v0.4.0. In particular, the Windows runtime gaps at the evaluated revision
were addressed subsequently in PR #79.

From the repository root, with a version-matched LLVM installation:

```sh
export LLVM_PREFIX=/opt/homebrew/opt/llvm@21
cmake -S . -B build_paper \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm" \
  -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
  -DKAGURA_BUILD_TESTS=ON -DKAGURA_BITCODE_TOOLS=ON
cmake --build build_paper -j 6
python3 paper/evaluate.py --llvm-bin "$LLVM_PREFIX/bin" --build build_paper
python3 paper/render_results.py
```

The runner uses only Python's standard library and measures native Mach-O
`__text` sections. Intermediate IR, objects, and executables stay in the
ignored `build_paper/paper-eval/` directory. Run it outside a debugger:
the shipped anti-debug constructor is intentionally included.

`results/current_eval.json` contains all 19 observations: one baseline, nine
shipped profile/seed combinations, and nine explicit anti-debug ablations.
Runtime failures are recorded and never counted as matches. Build failures
stop the experiment. A zero runner exit status means the measurement procedure
finished; inspect `selftest_passed`, `differential_passed`, and execution return
codes to determine whether a particular configuration passed.

New runs use report schema version 2 and atomically replace the selected JSON
with `status: "running"` before invoking external tools. Successful completion
sets `status: "complete"`; build, tool, baseline, or repeatability failures set
`status: "failed"`, retain completed observations and commands, and exit with
an error. An interrupted run stays non-complete. The table renderer rejects
non-complete reports and removes an existing output table before rendering,
so failed reruns cannot silently reuse earlier measurements.

The checked-in historical schema-version-1 report is also supported. Its input
hashes identify the original measurement-time sources, including the earlier
runner; later fixes to report handling do not change those observations.
The exact original runner is retained as
`results/provenance/evaluate-v1.py.txt` for checking its recorded hash.
The caption and observation counts are derived from the report's actual seeds.
To use a different seed set without overwriting the paper's historical data:

```sh
python3 paper/evaluate.py --llvm-bin "$LLVM_PREFIX/bin" --build build_paper \
  --seeds 8 43 --output build_paper/profile-eval.json
python3 paper/render_results.py --input build_paper/profile-eval.json \
  --output build_paper/profile-table.tex
```

Run the tool regression checks without LLVM or LaTeX:

```sh
python3 -m unittest discover -s tests/tooling -p 'test_paper_*.py' -v
```

The saved result shows:

- All nine shipped configurations abort before driver output on this host.
- All nine anti-debug ablations pass seven self-tests and match all 4,096
  return values per executable. The same input sequence is used each time.
- All 18 protected configurations produce identical IR in their two runs.
- Baseline acceptance counts for score, item, movement, and token are
  456, 332, 480, and 0. Token acceptance is not covered.

`benchmarks/differential_main.c` bounds its inputs to avoid signed overflow in
the existing movement subject. The baseline was also checked with:

```sh
"$LLVM_PREFIX/bin/clang" -O1 -fsanitize=undefined -fno-sanitize-recover=all \
  scripts/eval/benchmarks/eval_functions.c paper/benchmarks/differential_main.c \
  -o build_paper/differential-ubsan
build_paper/differential-ubsan
```

The six runtime-probe observations in `results/runtime_probe.txt` can be
reproduced with:

```sh
"$LLVM_PREFIX/bin/clang" -O1 paper/benchmarks/runtime_probe.c \
  build_paper/runtime/libkagura_runtime.a -o build_paper/runtime_probe
build_paper/runtime_probe
```

The GOT-hook probe reports 1 on the evaluation host; the other probes report
0. This is a diagnostic observation, not evidence that an attack occurred or
a measured false-positive rate across devices.

## Existing-suite validation

`results/validation.json` retains commands and logs for 84 passing CTest
entries, eight passing existing differential subjects, and the existing IR
reproducibility check. One CTest entry contains 48 nested FileCheck cases;
do not add 48 to the CTest count. Configure with a working `lit` and FileCheck
to include that suite, for example:

```sh
cmake -S . -B build_paper \
  -DLIT_EXECUTABLE="$PWD/.venv/bin/lit" \
  -DFILECHECK_EXECUTABLE="$LLVM_PREFIX/bin/FileCheck" \
  -DKAGURA_REQUIRE_LIT=ON
ctest --test-dir build_paper --output-on-failure -j 6
LLVM_PREFIX="$LLVM_PREFIX" bash scripts/ci/differential-test.sh \
  build_paper/lib/Transforms/KaguraObfuscator.dylib
LLVM_PREFIX="$LLVM_PREFIX" bash scripts/ci/verify-reproducible.sh \
  build_paper/lib/Transforms/KaguraObfuscator.dylib
```

## Selected-pass study

The independent runner `scripts/eval/benchmarks/run_eval.py` records Core and
Extended configurations, three seeds, seven self-tests, and `llc -O=1` code
generation. Its data and tables are in `results/native/`. Reproduce with:

```sh
python3 scripts/eval/benchmarks/run_eval.py \
  --llvm-prefix "$LLVM_PREFIX" \
  --plugin build_paper/lib/Transforms/KaguraObfuscator.dylib
```

Core and Extended are experimental pass lists, not the shipped BALANCED and
STRONG presets. Their instruction counts cover the four subject functions;
their executable text sizes include the harness. The current-profile study
uses Clang `-O0` lowering and reports both subject-object and executable text.

## Evidence boundaries

The revision removes unsupported analyst-hour predictions and branch-count
verdicts as security evidence, corrects the description of O-MVLL as open
source, and distinguishes implemented runtime checks from stubs. It reports
the startup incompatibility without modifying the runtime to hide it.
No runtime-performance, production-device, or attack-resistance claim is
established by these measurements. `results/eval_summary.json` is historical
and is not used by the revised paper.
