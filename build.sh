#!/usr/bin/env bash
#
# One-shot build for the kagura plugin, runtime, kagura-opt and the test suite.
#
# Usage:
#   bash build.sh [BUILD_DIR] [extra cmake args...]
#
# Environment:
#   LLVM_PREFIX   LLVM installation prefix. Auto-detected if unset:
#                 supported llvm-config tools, Homebrew or /usr/lib/llvm-*.
#   JOBS          Parallel build jobs (default: all logical CPUs).
#
# Examples:
#   bash build.sh
#   bash build.sh build-debug -DCMAKE_BUILD_TYPE=Debug
#   LLVM_PREFIX=/usr/lib/llvm-19 bash build.sh

set -euo pipefail

# ---- Build directory (first positional arg), remaining args go to cmake ------
BUILD_DIR="build"
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then
  BUILD_DIR="$1"
  shift
fi

# ---- Locate LLVM -------------------------------------------------------------
# llvm-config and Homebrew's unversioned llvm may point to a newer major
# even when a supported version is installed alongside it.
supported_prefix() {
  local prefix="$1" version
  [[ -f "$prefix/lib/cmake/llvm/LLVMConfig.cmake" &&
     -x "$prefix/bin/clang" && -x "$prefix/bin/clang++" ]] || return 1
  version=$("$prefix/bin/llvm-config" --version 2>/dev/null) || return 1
  [[ "$version" =~ ^(17|18|19|20|21|22)\. ]]
}

find_llvm_prefix() {
  local candidates=()
  local tool prefix brew_prefix
  for tool in llvm-config llvm-config-{22..17}; do
    if prefix=$("$tool" --prefix 2>/dev/null); then
      candidates+=("$prefix")
    fi
  done
  if brew_prefix=$(brew --prefix 2>/dev/null); then
    candidates+=("$brew_prefix/opt/llvm" "$brew_prefix"/opt/llvm@{22..17})
  fi
  candidates+=(/usr/lib/llvm-{22..17} /usr/local /usr)

  for prefix in "${candidates[@]}"; do
    if supported_prefix "$prefix"; then
      echo "$prefix"
      return
    fi
  done
  return 1
}

if [[ -z "${LLVM_PREFIX:-}" ]]; then
  LLVM_PREFIX=$(find_llvm_prefix) || {
    echo "error: could not find LLVM 17–22. Set LLVM_PREFIX to a matching LLVM installation." >&2
    exit 1
  }
elif [[ ! -f "$LLVM_PREFIX/lib/cmake/llvm/LLVMConfig.cmake" ]]; then
  echo "error: LLVM_PREFIX=$LLVM_PREFIX does not contain lib/cmake/llvm/LLVMConfig.cmake." >&2
  exit 1
fi

# ---- Parallelism (macOS has no nproc, Linux has no sysctl -n hw.logicalcpu) --
if [ -z "${JOBS:-}" ]; then
  if command -v nproc > /dev/null 2>&1; then
    JOBS="$(nproc)"
  elif command -v sysctl > /dev/null 2>&1; then
    JOBS="$(sysctl -n hw.logicalcpu)"
  else
    JOBS=4
  fi
fi

echo "LLVM prefix : $LLVM_PREFIX"
echo "Build dir   : $BUILD_DIR"
echo "Jobs        : $JOBS"
echo ""

# KAGURA_BITCODE_TOOLS=ON is what actually produces kagura-opt; without it the
# documented one-shot build never builds the tool the docs tell you to run.
cmake -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" \
  -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm" \
  -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DKAGURA_BITCODE_TOOLS=ON \
  -DKAGURA_BUILD_TESTS=ON \
  "$@" \
  .

cmake --build "$BUILD_DIR" -j"$JOBS"

echo ""
if [ -x "$BUILD_DIR/bin/kagura" ]; then
  echo "Usage:"
  printf '  %q --clang %q -kagura-config=integration/profiles/balanced.json -O1 your_file.c -o your_file\n' \
    "$BUILD_DIR/bin/kagura" "$LLVM_PREFIX/bin/clang"
fi
echo "Tests:"
printf '  ctest --test-dir %q --output-on-failure\n' "$BUILD_DIR"
