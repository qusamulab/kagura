#!/usr/bin/env bash
# Reproducible native size/correctness evaluation; see paper/README.md.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "${PYTHON:-python3}" "$SCRIPT_DIR/run_eval.py" "$@"
