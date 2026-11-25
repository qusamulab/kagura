"""Policy and audit contracts through real standalone/named pass pipelines."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--tool", required=True)
parser.add_argument("--opt")
parser.add_argument("--plugin")
options, remaining = parser.parse_known_args()


class PipelineContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kagura-pipeline-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "input.ll"
        self.source.write_text(r'''
@secret = private constant [23 x i8] c"allowed string literal\00"
declare void @use(ptr)
define i32 @arith(i32 %a, i32 %b) noinline {
 %x = add i32 %a, %b
 ret i32 %x
}
define i32 @excluded(i32 %a, i32 %b) noinline {
 %x = add i32 %a, %b
 ret i32 %x
}
define void @consumer() noinline {
 call void @use(ptr @secret)
 ret void
}
''')

    def run_tool(self, *args, success=True):
        result = subprocess.run([options.tool, *map(str, args), "-S", str(self.source),
                                 "-o", str(self.root / "output.ll")],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result

    def test_requested_policy_errors_fail_without_strict_mode(self):
        policy = self.root / "policy.json"
        for text in (None, "{invalid", "[]"):
            with self.subTest(text=text):
                if text is not None:
                    policy.write_text(text)
                self.run_tool(f"-kagura-config={policy}", success=False)
                self.assertFalse((self.root / "output.ll").exists())

    def test_strong_profile_uses_aes_for_narrow_strings(self):
        policy = self.root / "policy.json"
        policy.write_text(json.dumps({"profile": "STRONG",
                                      "passes": {"anti_debug": False, "tamper": False}}))
        self.run_tool("-O1", f"-kagura-config={policy}")
        ir = (self.root / "output.ll").read_text()
        self.assertIn("@kagura_aesenc_", ir)
        self.assertNotIn("@kagura_enc_", ir)
        self.assertNotIn('c"allowed string literal', ir)

    def check_audit(self, record):
        results = {(r["pass"], r["scope"], r["name"]): r["status"]
                   for r in record["pass_results"]}
        self.assertEqual(results[("kagura-sub", "function", "arith")], "changed")
        self.assertEqual(results[("kagura-sub", "function", "excluded")], "unchanged")
        self.assertEqual(results[("kagura-str", "function", "consumer")], "changed")
        functions = {f["name"]: f["passes"] for f in record["functions"]}
        self.assertIn("sub", functions["arith"])
        self.assertIn("str", functions["consumer"])
        self.assertNotIn("excluded", functions)

    def test_automatic_pipeline_records_changes_and_exclusions(self):
        audit = self.root / "audit.jsonl"
        self.run_tool("-O1", "-kagura-sub", "-kagura-str", "-kagura-audit",
                      "-kagura-deny=excluded", f"-kagura-audit-out={audit}")
        self.check_audit(json.loads(audit.read_text()))

    @unittest.skipUnless(options.plugin and options.opt, "named pipelines need a loadable plugin")
    def test_named_pipeline_records_function_and_module_changes(self):
        audit = self.root / "named.jsonl"
        result = subprocess.run([options.opt, f"-load-pass-plugin={options.plugin}",
                                 "-kagura-audit", f"-kagura-audit-out={audit}",
                                 "-kagura-deny=excluded",
                                 "-passes=function(kagura-sub),kagura-str,kagura-audit,verify",
                                 "-disable-output", str(self.source)],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.check_audit(json.loads(audit.read_text()))

    def test_audit_records_unchanged_and_o0_paths(self):
        for flags in (("-O1", "-kagura-sub", "-kagura-deny=*"),
                      ("-O0", "-kagura-o0-protect", "-kagura-str")):
            audit = self.root / "audit.jsonl"
            audit.unlink(missing_ok=True)
            self.run_tool(*flags, "-kagura-audit", f"-kagura-audit-out={audit}")
            record = json.loads(audit.read_text())
            self.assertTrue(record["pass_results"])
            if "-O1" in flags:
                self.assertFalse(record["functions"])
                self.assertTrue(all(r["status"] == "unchanged" for r in record["pass_results"]))
            else:
                self.assertTrue(any(r["pass"] == "kagura-str" and r["status"] == "changed"
                                    for r in record["pass_results"]))

    def test_parallel_reports_remain_complete_json_lines(self):
        audit = self.root / "audit.jsonl"
        symbols = self.root / "symbols.jsonl"
        # Each symbol record spans multiple output buffers.
        globals_ir = "".join(f"@kagura_report_{i} = global i32 {i}\n" for i in range(200))
        input_ir = self.source.read_text() + globals_ir

        def compile_one(index):
            source = self.root / f"input-{index}.ll"
            source.write_text(input_ir)
            result = subprocess.run([options.tool, "-O1", "-kagura-sub", "-kagura-audit",
                                     f"-kagura-audit-out={audit}", "-kagura-symmap",
                                     f"-kagura-symmap-out={symbols}", str(source),
                                     "-o", str(self.root / f"out-{index}.bc")],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)

        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(compile_one, range(12)))
        for path, field in ((audit, "functions"), (symbols, "symbols")):
            records = [json.loads(line) for line in path.read_text().splitlines()]
            self.assertCountEqual((Path(r["module"]).name for r in records),
                                  (f"input-{i}.ll" for i in range(12)))
            self.assertTrue(all(r[field] for r in records))

        cli = Path(__file__).resolve().parents[2] / "scripts/cli/kagura-cli.py"
        result = subprocess.run([sys.executable, str(cli), "symmap-view", "--map", str(symbols),
                                 "--filter", "kagura_report_"], capture_output=True, text=True,
                                timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Total: 2400 symbol(s)", result.stdout)

    def test_requested_symbol_path_errors_fail_build(self):
        missing = self.root / "missing" / "symbols.jsonl"
        result = self.run_tool("-O1", "-kagura-str", "-kagura-symmap",
                               f"-kagura-symmap-out={missing}", success=False)
        self.assertIn("cannot open symbol map", result.stderr)
        self.assertIn(str(missing), result.stderr)

    def test_requested_audit_path_errors_fail_build(self):
        self.run_tool("-O1", "-kagura-sub", "-kagura-audit",
                      f"-kagura-audit-out={self.root / 'missing' / 'audit.jsonl'}", success=False)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
