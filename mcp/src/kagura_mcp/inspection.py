"""Read-only workspace inspection, shared by the MCP tools and tests."""

from __future__ import annotations

from collections import Counter
from pathlib import Path
import re
import shutil
import subprocess

from .binary import collect, structured_report
from .policy import load_config, validate_config


class Workspace:
    def __init__(self, root: Path):
        self.root = root.expanduser().resolve(strict=True)
        if not self.root.is_dir():
            raise ValueError("Workspace must be a directory.")

    def file(self, path: str, max_bytes: int = 16 * 1024 * 1024) -> Path:
        candidate = Path(path).expanduser()
        candidate = (self.root / candidate).resolve(strict=True)
        if not candidate.is_relative_to(self.root):
            raise ValueError("Path must stay inside the configured workspace (including symlinks).")
        if not candidate.is_file():
            raise ValueError("Expected a regular file.")
        if candidate.stat().st_size > max_bytes:
            raise ValueError(f"File exceeds the {max_bytes}-byte inspection limit.")
        return candidate

    def validate(self, path: str) -> dict:
        source = self.file(path)
        try:
            return {"path": str(source), **validate_config(load_config(source.read_text(encoding="utf-8")))}
        except (ValueError, UnicodeError) as exc:
            return {"path": str(source), "valid": False,
                    "errors": [{"path": "$", "message": str(exc)}]}

    def audit(self, path: str, module: str = "", function: str = "",
              offset: int = 0, limit: int = 100) -> dict:
        if offset < 0 or not 1 <= limit <= 500:
            raise ValueError("offset must be >= 0 and limit must be in [1, 500].")
        source = self.file(path)
        rows, errors, counts = [], [], Counter()
        records = total_errors = 0
        for line_number, line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            if not line.strip():
                continue
            try:
                record = load_config(line)
                if not isinstance(record, dict) or not isinstance(record.get("module"), str):
                    raise ValueError("Expected a record with a module string.")
                functions = record.get("functions")
                if not isinstance(functions, list):
                    raise ValueError("Expected a functions array.")
                for item in functions:
                    if (not isinstance(item, dict) or not isinstance(item.get("name"), str)
                            or not isinstance(item.get("passes"), list)
                            or any(not isinstance(p, str) for p in item["passes"])):
                        raise ValueError("Expected function objects with name and passes strings.")
                if module and module not in record["module"]:
                    continue
                records += 1
                for item in functions:
                    if function and function not in item["name"]:
                        continue
                    counts.update(item["passes"])
                    rows.append({"module": record["module"], "name": item["name"],
                                 "passes": item["passes"], "build_id": record.get("build_id", "")})
            except (ValueError, TypeError) as exc:
                total_errors += 1
                if len(errors) < 50:
                    errors.append({"line": line_number, "message": str(exc)})
        end = offset + limit
        return {"path": str(source), "complete": not total_errors,
                "record_count": records, "function_entries": len(rows),
                "pass_counts": dict(counts), "functions": rows[offset:end],
                "next_offset": end if end < len(rows) else None,
                "error_count": total_errors, "errors": errors,
                "notes": ["Only metadata-marked functions appear; absence is not proof a function is unprotected.",
                          "Counts are log entries; appended builds can repeat a function."]}

    def compare(self, baseline: str, obfuscated: str, min_string_length: int = 4,
                sample_limit: int = 50) -> dict:
        if not 1 <= min_string_length <= 128 or not 0 <= sample_limit <= 200:
            raise ValueError("min_string_length must be in [1, 128]; sample_limit in [0, 200].")
        before = self.file(baseline, 256 * 1024 * 1024)
        after = self.file(obfuscated, 256 * 1024 * 1024)
        result = structured_report(collect(before, min_string_length),
                                   collect(after, min_string_length), sample_limit)
        return {**result, "min_string_length": min_string_length}


def doctor(workspace: Workspace, llvm_bin: Path | None = None,
           plugin: Path | None = None) -> dict:
    tools = {}
    warnings = []
    for name in ("clang", "llvm-config", "opt", "kagura-opt", "kagura", "llvm-nm", "nm", "llvm-size", "size", "strings"):
        candidates = []
        if llvm_bin:
            candidates.append(llvm_bin / name)
        if name in {"kagura-opt", "kagura"}:
            candidates.append(workspace.root / "build" / "bin" / name)
        path = next((str(p) for p in candidates if p.is_file()), None) or shutil.which(name)
        # Windows' native find-on-PATH appends .exe; also support explicit --llvm-bin.
        if not path:
            path = next((str(p.with_suffix(ext)) for p in candidates
                         for ext in ((".cmd",) if name == "kagura" else (".exe",))
                         if p.with_suffix(ext).is_file()), None)
        tools[name] = {"path": path}
        if path and name in ("clang", "llvm-config", "opt", "kagura-opt"):
            args = [path]
            if name == "kagura-opt" and plugin:
                args.append(f"-load-kagura={plugin}")
            args.append("--version")
            try:
                run = subprocess.run(args, capture_output=True, text=True, errors="replace", timeout=10)
                output = (run.stdout + run.stderr).strip()
                tools[name].update({"ok": run.returncode == 0, "version": output[:2000]})
                if match := re.search(r"(?:LLVM version|clang version|^)(\s*)(\d+)\.", output):
                    tools[name]["major"] = int(match.group(2))
            except (OSError, subprocess.TimeoutExpired) as exc:
                tools[name].update({"ok": False, "error": str(exc)})
    majors = {entry["major"] for entry in tools.values() if "major" in entry}
    if len(majors) > 1:
        warnings.append("Discovered LLVM tools have different major versions; align clang, opt and the plugin ABI.")
    if any(major < 17 or major > 22 for major in majors):
        warnings.append("Supported upstream LLVM versions are 17–22; Apple clang version numbers do not establish ABI compatibility.")
    if not tools["kagura-opt"]["path"]:
        warnings.append("kagura-opt was not found; build with -DKAGURA_BITCODE_TOOLS=ON for standalone IR processing.")
    elif not tools["kagura-opt"].get("ok"):
        warnings.append("kagura-opt could not start/load its plugin; inspect its version diagnostic and rebuild with a matching LLVM toolchain.")
    if not tools["kagura"]["path"]:
        warnings.append("kagura compiler driver was not found; build/install the default bitcode tools, or use kagura-opt directly.")
    warnings.append("Use kagura with matching --clang and --kagura-opt for source compilation on LLVM 17–22; direct clang -fpass-plugin -mllvm flags require LLVM 22.")
    return {"workspace": str(workspace.root), "tools": tools,
            "plugin": str(plugin) if plugin else None,
            "policy_tools_ready": True,
            "kagura_opt_ready": tools["kagura-opt"].get("ok", False),
            "compiler_driver_ready": bool(tools["kagura"]["path"] and
                                          tools["kagura-opt"].get("ok") and
                                          tools["clang"].get("ok") and
                                          tools["clang"].get("major") in range(17, 23) and
                                          tools["clang"].get("major") == tools["kagura-opt"].get("major")
                                          and "Apple clang" not in tools["clang"].get("version", "")),
            "binary_comparison_ready": bool((tools["llvm-nm"]["path"] or tools["nm"]["path"])
                                             and (tools["llvm-size"]["path"] or tools["size"]["path"])
                                             and tools["strings"]["path"]),
            "warnings": warnings}
