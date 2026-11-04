#!/usr/bin/env python3
"""Measure current shipped profiles on the paper subjects (native macOS).

No cost-model or resistance verdicts are produced. Build failures stop the run;
runtime failures are recorded explicitly, never counted as passing cases.
Commands, hashes, inputs, seed values and individual observations are retained.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_report(path, record):
    """Publish a complete JSON document even when a previous run failed."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                     prefix=path.name + ".", suffix=".tmp",
                                     delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(record, stream, indent=2)
            stream.write("\n")
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def run_evaluation(args, record):
    build = args.build.resolve()
    llvm = args.llvm_bin.resolve()
    work = build / "paper-eval"
    work.mkdir(parents=True, exist_ok=True)
    plugin = build / "lib/Transforms/KaguraObfuscator.dylib"
    runtime = build / "runtime/libkagura_runtime.a"
    clang, opt, size = [llvm / x for x in ("clang", "opt", "llvm-size")]
    commands = record["commands"]

    def process(cmd, timeout=120):
        cmd = [str(x) for x in cmd]
        commands.append(cmd)
        return subprocess.run(cmd, cwd=ROOT, text=True,
                              capture_output=True, timeout=timeout)

    def run(cmd, timeout=120):
        result = process(cmd, timeout)
        result.check_returncode()
        return result.stdout

    def text_size(path):
        report = run([size, "-A", path])
        match = re.search(r"^__text\s+(\d+)\s+", report, re.M)
        if not match:
            raise RuntimeError(f"No __text section in {path}: {report}")
        return int(match.group(1))

    subjects = ROOT / "scripts/eval/benchmarks/eval_functions.c"
    selftest = ROOT / "scripts/eval/benchmarks/eval_main.c"
    differential = ROOT / "paper/benchmarks/differential_main.c"
    inputs = [subjects, selftest, differential, Path(__file__),
              ROOT / "include/kagura/PassRegistry.def",
              ROOT / "lib/Transforms/Profiles.def"]
    profiles = {name: ROOT / f"integration/profiles/{name.lower()}.json"
                for name in ("FAST", "BALANCED", "STRONG")}
    inputs += list(profiles.values())
    record.update({
        "source_commit": run(["git", "rev-parse", "HEAD"]).strip(),
        "host": {"os": run(["sw_vers", "-productVersion"]).strip(),
                 "cpu": run(["sysctl", "-n", "machdep.cpu.brand_string"]).strip(),
                 "architecture": platform.machine()},
        "clang_version": run([clang, "--version"]).strip(),
        "plugin_sha256": sha(plugin), "runtime_sha256": sha(runtime),
        "input_sha256": {str(p.relative_to(ROOT)): sha(p) for p in inputs},
        "profile_settings": {n: json.loads(p.read_text()) for n, p in profiles.items()},
        "method": {
            "ir_optimization": "clang -O1 -Xclang -disable-llvm-passes; opt default<O1>",
            "code_generation": "clang -O0 on optimized/transformed IR",
            "driver": "compiled separately with clang -O1, no obfuscation",
            "runtime_linkage": "shipped static runtime for protected profiles only",
            "selftests": 7, "differential_inputs_per_function": 1024,
            "functions": 4, "seeds": args.seeds,
            "reproducibility": "two separate opt processes per protected configuration",
            "scope": "correctness, code size, IR repeatability; no security or timing metric",
            "ablation": "each shipped profile also built with passes.anti_debug=false"
        },
    })
    raw = work / "subjects.ll"
    run([clang, "-O1", "-Xclang", "-disable-llvm-passes", "-S", "-emit-llvm",
         subjects, "-o", raw])
    for source, name in [(selftest, "selftest"), (differential, "differential")]:
        run([clang, "-O1", "-c", source, "-o", work / f"{name}.o"])

    baseline = None
    configurations = [("plain", False)] + [(name, ablate)
                        for ablate in (False, True) for name in profiles]
    for name, ablate in configurations:
        for seed in ([None] if name == "plain" else args.seeds):
            label = name.lower() + ("-no-antidebug" if ablate else "")
            prefix = work / f"{label}-{seed}"
            ir = prefix.with_suffix(".ll")
            cmd = [opt]
            if name != "plain":
                policy = profiles[name]
                if ablate:
                    settings = json.loads(policy.read_text())
                    settings["passes"]["anti_debug"] = False
                    policy = work / f"{label}.json"
                    policy.write_text(json.dumps(settings, indent=2) + "\n")
                cmd += [f"-load-pass-plugin={plugin}",
                        f"-kagura-config={policy}", "-kagura-config-strict",
                        f"-kagura-seed={seed}"]
            cmd += ["-passes=default<O1>", "-verify-each", "-S", raw, "-o", ir]
            run(cmd)
            repeated = None
            if name != "plain":
                repeat = prefix.with_suffix(".repeat.ll")
                run(cmd[:-1] + [repeat])
                repeated = ir.read_bytes() == repeat.read_bytes()
                if not repeated:
                    raise RuntimeError(f"Nonrepeatable IR: {name}, seed {seed}")
            obj = prefix.with_suffix(".o")
            run([clang, "-O0", "-c", ir, "-o", obj])
            row = {"profile": name, "anti_debug_disabled": ablate,
                   "seed": seed, "ir_sha256": sha(ir),
                   "ir_repeated_identically": repeated,
                   "subject_text_bytes": text_size(obj)}
            outputs = {}
            for driver in ("selftest", "differential"):
                binary = Path(str(prefix) + "-" + driver)
                link = [clang, work / f"{driver}.o", obj]
                if name != "plain":
                    link.append(runtime)
                run(link + ["-o", binary])
                try:
                    execution = process([binary], timeout=60)
                    outputs[driver] = execution.stdout
                    row[driver + "_execution"] = {
                        "returncode": execution.returncode,
                        "stderr": execution.stderr,
                        "timed_out": False}
                except subprocess.TimeoutExpired:
                    outputs[driver] = ""
                    row[driver + "_execution"] = {"returncode": None, "timed_out": True}
                if driver == "selftest":
                    row.update({"selftest_output": outputs[driver].strip(),
                                "executable_bytes": binary.stat().st_size,
                                "executable_text_bytes": text_size(binary)})
            row["selftest_passed"] = (row["selftest_execution"]["returncode"] == 0
                                      and outputs["selftest"].strip() == "Self-test: 7/7 passed")
            if baseline is None:
                if not row["selftest_passed"]:
                    raise RuntimeError("Baseline self-test failed")
                if row["differential_execution"]["returncode"] != 0:
                    raise RuntimeError("Baseline differential execution failed")
                baseline = outputs["differential"]
                lines = [list(map(int, line.split())) for line in baseline.splitlines()]
                if len(lines) != 1024 or any(len(line) != 4 for line in lines):
                    raise RuntimeError("Unexpected differential output shape")
                record["baseline_accept_counts"] = [sum(line[i] for line in lines)
                                                     for i in range(4)]
            row["differential_passed"] = (row["differential_execution"]["returncode"] == 0
                                          and outputs["differential"] == baseline)
            row["differential_return_values_matched"] = 4096 if row["differential_passed"] else None
            row["differential_output_sha256"] = hashlib.sha256(
                outputs["differential"].encode()).hexdigest()
            record["observations"].append(row)
            print(json.dumps(row), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", required=True, type=Path)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "paper/results/current_eval.json")
    parser.add_argument("--seeds", nargs="+", type=int, default=[7, 42, 12345])
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("This experiment measures Mach-O __text on native macOS.")
    if len(set(args.seeds)) != len(args.seeds) or any(
            seed <= 0 or seed >= 2**64 for seed in args.seeds):
        parser.error("provide distinct nonzero uint64 seeds")
    args.output = args.output.resolve()
    record = {"schema_version": 2, "status": "running",
              "started_at_utc": datetime.now(timezone.utc).isoformat(),
              "observations": [], "commands": []}
    # Invalidate the previous result before any external tools can fail.
    write_report(args.output, record)
    try:
        run_evaluation(args, record)
    except BaseException as exc:
        record.update({"status": "failed", "error": {
            "type": type(exc).__name__, "message": str(exc)}})
        if getattr(exc, "stderr", None):
            stderr = exc.stderr
            record["error"]["stderr"] = (stderr.decode(errors="replace")
                                            if isinstance(stderr, bytes) else stderr)
        record["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
        write_report(args.output, record)
        raise
    record["status"] = "complete"
    record["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
    write_report(args.output, record)


if __name__ == "__main__":
    main()
