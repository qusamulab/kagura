#!/usr/bin/env python3
"""Compare two Kagura builds; implementation is shared with kagura-mcp.

Usage: kagura-diff.py baseline obfuscated [--html report.html] [--json report.json]
"""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "mcp" / "src"))
from kagura_mcp.binary import main

if __name__ == "__main__":
    sys.exit(main())
