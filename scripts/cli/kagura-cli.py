#!/usr/bin/env python3
"""
kagura-cli.py — Kagura configuration generator and audit log viewer.

Usage:
  kagura-cli.py config-gen [--profile PROFILE] [--passes PASS,...] \
                            [--protect PATTERN,...] [--deny PATTERN,...] \
                            [--allow PATTERN,...] [-o OUTPUT.json]

  kagura-cli.py audit-view [--log AUDIT.json] [--module FILTER] [--sort-passes]

  kagura-cli.py symmap-view [--map SYMBOLS.json] [--filter PATTERN]

Sub-commands
------------
config-gen    Generate a kagura.json policy file from the shared registry.
config-check  Lint a policy without compiling or loading LLVM.
audit-view    Pretty-print / filter the audit log produced by -kagura-audit.
symmap-view   Pretty-print / filter the symbol map produced by -kagura-symmap.
"""

import argparse
import json
import sys
import os
from pathlib import Path
from datetime import datetime

# Use the source checkout's shared library without requiring the MCP SDK.
# Installed users can import the same library from the kagura-mcp wheel.
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "mcp" / "src"))
from kagura_mcp.policy import generate_config, load_config, validate_config


def cmd_config_gen(args):
    def patterns(value):
        return [p.strip() for p in value.split(",")] if value is not None else None

    if args.audit_out and not args.audit:
        raise ValueError("--audit-out requires --audit")
    if args.symmap_out and not args.symmap:
        raise ValueError("--symmap-out requires --symmap")
    passes = {p.strip(): True for p in args.passes.split(",")} if args.passes else {}
    if args.dwarf:
        passes["dwarf"] = args.dwarf
    result = generate_config(
        profile=args.profile or "CUSTOM", passes=passes,
        tuning={"seed": int(args.seed)} if args.seed is not None else None,
        protect=patterns(args.protect), denylist=patterns(args.deny), allowlist=patterns(args.allow),
        aggressive_strings=args.aggressive_strings, scoped_strings=args.scoped_strings,
    )
    cfg = result["config"]
    flags = []
    if args.build_id:
        flags.append("-kagura-build-id=" + args.build_id)
    if args.audit:
        flags.append("-kagura-audit")
        if args.audit_out:
            cfg["audit_out"] = args.audit_out
    if args.symmap:
        flags.append("-kagura-symmap")
        if args.symmap_out:
            flags.append("-kagura-symmap-out=" + args.symmap_out)
    text = json.dumps(cfg, indent=2)
    if args.output:
        Path(args.output).write_text(text + "\n", encoding="utf-8")
        print(f"Config written to {args.output}")
    else:
        print(text)
    if flags:
        print("Additional compiler flags (these cannot be enabled in JSON): " +
              json.dumps(flags), file=sys.stderr)
    for note in result["notes"]:
        print(note, file=sys.stderr)


def cmd_config_check(args):
    try:
        result = validate_config(load_config(Path(args.config).read_text(encoding="utf-8")))
    except (ValueError, UnicodeError) as exc:
        result = {"valid": False, "errors": [{"path": "$", "message": str(exc)}]}
    print(json.dumps(result, indent=2))
    return 0 if result["valid"] else 1

# ── Sub-command: audit-view ───────────────────────────────────────────────────

def cmd_audit_view(args):
    log_path = args.log or "kagura_audit.json"
    if not os.path.exists(log_path):
        print(f"Audit log not found: {log_path}", file=sys.stderr)
        sys.exit(1)

    records = []
    with open(log_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"Parse error: {e}", file=sys.stderr)

    if args.module_filter:
        records = [r for r in records if args.module_filter in r.get("module", "")]

    total_funcs = 0
    for rec in records:
        ts = datetime.utcfromtimestamp(rec.get("timestamp", 0)).strftime("%Y-%m-%d %H:%M:%S")
        module = rec.get("module", "<unknown>")
        build_id = rec.get("build_id", "")
        funcs = rec.get("functions", [])
        total_funcs += len(funcs)
        print(f"\n{'='*60}")
        print(f"Module   : {module}")
        print(f"Time     : {ts} UTC")
        if build_id:
            print(f"Build ID : {build_id}")
        print(f"Functions: {len(funcs)}")

        if args.sort_passes:
            from collections import Counter
            all_passes = [p for fn in funcs for p in fn.get("passes", [])]
            pass_counts = Counter(all_passes)
            print(f"Passes   : {dict(pass_counts.most_common())}")
        else:
            for fn in funcs:
                passes = ", ".join(fn.get("passes", []))
                print(f"  {fn.get('name', '?')} → [{passes}]")

    print(f"\nTotal: {total_funcs} protected function(s) across {len(records)} module(s)")

# ── Sub-command: symmap-view ──────────────────────────────────────────────────

def cmd_symmap_view(args):
    map_path = args.map or "kagura_symbols.json"
    if not os.path.exists(map_path):
        print(f"Symbol map not found: {map_path}", file=sys.stderr)
        sys.exit(1)

    with open(map_path) as f:
        data = json.load(f)

    symbols = data.get("symbols", data) if isinstance(data, dict) else data
    if isinstance(symbols, dict):
        symbols = [{"original": k, "obfuscated": v} for k, v in symbols.items()]

    if args.filter:
        symbols = [s for s in symbols
                   if args.filter in s.get("original", "") or
                      args.filter in s.get("obfuscated", "")]

    print(f"{'Original':<50} {'Obfuscated'}")
    print("-" * 80)
    for s in symbols:
        orig = s.get("original", "?")
        obfs = s.get("obfuscated", "?")
        print(f"{orig:<50} {obfs}")
    print(f"\nTotal: {len(symbols)} symbol(s)")

# ── Argument parsing ──────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Kagura CLI — config generator and report viewer",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command")

    # config-gen
    cg = sub.add_parser("config-gen", help="Generate kagura.json policy file")
    cg.add_argument("--profile", help="Preset profile: FAST | BALANCED | STRONG")
    cg.add_argument("--passes",  help="Comma-separated list of passes to enable")
    cg.add_argument("--protect", help="Comma-separated force-protect patterns")
    cg.add_argument("--deny",    help="Comma-separated denylist patterns")
    cg.add_argument("--allow",   help="Comma-separated allowlist patterns")
    cg.add_argument("--build-id", dest="build_id", help="Build ID string")
    cg.add_argument("--seed",    help="PRNG seed (integer)")
    cg.add_argument("--audit",   action="store_true", help="Enable audit log")
    cg.add_argument("--audit-out", dest="audit_out", help="Audit log output path")
    cg.add_argument("--symmap",  action="store_true", help="Enable symbol map")
    cg.add_argument("--symmap-out", dest="symmap_out", help="Symbol map output path")
    cg.add_argument("--dwarf",   choices=["keep","strip","obfuscate"], help="DWARF mode")
    cg.add_argument("--aggressive-strings", action="store_true", help="Protect short/format/startup strings")
    cg.add_argument("--scoped-strings", action="store_true", help="Wipe per-call plaintext when lifetime permits")
    cg.add_argument("-o", "--output", help="Output path (default: stdout)")

    cc = sub.add_parser("config-check", help="Validate a policy without LLVM or compilation")
    cc.add_argument("config", help="Policy JSON file")

    # audit-view
    av = sub.add_parser("audit-view", help="View kagura audit log")
    av.add_argument("--log",    help="Audit log path (default: kagura_audit.json)")
    av.add_argument("--module", dest="module_filter", help="Filter by module name")
    av.add_argument("--sort-passes", action="store_true", help="Show pass frequency")

    # symmap-view
    sv = sub.add_parser("symmap-view", help="View kagura symbol map")
    sv.add_argument("--map",    help="Symbol map path (default: kagura_symbols.json)")
    sv.add_argument("--filter", help="Filter by name substring")

    args = parser.parse_args()

    if args.command == "config-check":
        return cmd_config_check(args)
    if args.command == "config-gen":
        cmd_config_gen(args)
    elif args.command == "audit-view":
        cmd_audit_view(args)
    elif args.command == "symmap-view":
        cmd_symmap_view(args)
    else:
        parser.print_help()

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
