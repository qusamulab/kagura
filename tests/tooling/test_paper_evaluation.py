"""Regression checks for report freshness and data-driven paper tables.

These use temporary reports and simulated tool failure; no compiler, LaTeX,
macOS runtime, or external Python packages are needed.
"""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def load_script(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "paper" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


evaluate = load_script("evaluate")
render = load_script("render_results")


def report(seeds=(8, 43)):
    rows = []
    for profile, ablated in [("plain", False)] + [
            (name, ablated) for ablated in (False, True)
            for name in ("FAST", "BALANCED", "STRONG")]:
        for seed in ([None] if profile == "plain" else seeds):
            rows.append({"profile": profile, "anti_debug_disabled": ablated,
                         "seed": seed, "subject_text_bytes": 100,
                         "executable_text_bytes": 200, "executable_bytes": 300,
                         "selftest_passed": True, "differential_passed": True,
                         "selftest_execution": {"returncode": 0},
                         "differential_execution": {"returncode": 0}})
    return {"schema_version": 2, "status": "complete",
            "host": {"architecture": "arm64"},
            "method": {"seeds": list(seeds), "differential_inputs_per_function": 1024,
                       "functions": 4}, "observations": rows}


class PaperEvaluationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="kagura-paper-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.json_path = self.directory / "result.json"
        self.table_path = self.directory / "table.tex"

    def render_file(self):
        with patch.object(sys, "argv", ["render_results.py", "--input",
                                       str(self.json_path), "--output", str(self.table_path)]):
            render.main()

    def test_failed_rerun_invalidates_previous_result_and_table(self):
        self.json_path.write_text(json.dumps(report()))
        self.table_path.write_text("old successful table")
        commands_seen = []

        def missing_tool(argv, **kwargs):
            commands_seen.append(argv)
            # Model a real tool failure after metadata commands have succeeded.
            if Path(argv[0]).name == "clang":
                raise FileNotFoundError("LLVM executable was removed")
            return subprocess.CompletedProcess(argv, 0, stdout="test metadata\n", stderr="")

        with patch.object(sys, "argv", ["evaluate.py", "--llvm-bin", str(self.directory),
                                       "--build", str(self.directory / "build"),
                                       "--output", str(self.json_path)]), \
                patch.object(evaluate.platform, "system", return_value="Darwin"), \
                patch.object(evaluate.subprocess, "run", side_effect=missing_tool):
            with self.assertRaises(FileNotFoundError):
                evaluate.main()
        failed = json.loads(self.json_path.read_text())
        self.assertEqual(failed["status"], "failed")
        self.assertEqual(failed["observations"], [])
        self.assertEqual(failed["error"]["type"], "FileNotFoundError")
        self.assertTrue(failed["commands"])
        self.assertTrue(commands_seen)
        with self.assertRaisesRegex(ValueError, "incomplete evaluation"):
            self.render_file()
        self.assertFalse(self.table_path.exists())
        self.assertFalse(list(self.directory.glob("*.tmp")))

    def test_successful_run_transitions_from_running_to_complete(self):
        def finish(args, record):
            self.assertEqual(json.loads(args.output.read_text())["status"], "running")
            record.update({key: value for key, value in report().items()
                           if key not in ("status", "schema_version")})

        with patch.object(sys, "argv", ["evaluate.py", "--llvm-bin", str(self.directory),
                                       "--build", str(self.directory), "--seeds", "8", "43",
                                       "--output", str(self.json_path)]), \
                patch.object(evaluate.platform, "system", return_value="Darwin"), \
                patch.object(evaluate, "run_evaluation", side_effect=finish):
            evaluate.main()
        self.assertEqual(json.loads(self.json_path.read_text())["status"], "complete")
        self.render_file()
        self.assertIn("8 and 43", self.table_path.read_text())

    def test_interrupted_and_malformed_reports_remove_stale_table(self):
        for status in ("running", "failed", None):
            with self.subTest(status=status):
                data = report()
                data["status"] = status
                self.json_path.write_text(json.dumps(data))
                self.table_path.write_text("stale table")
                with self.assertRaises(ValueError):
                    self.render_file()
                self.assertFalse(self.table_path.exists())
        self.json_path.write_text("{invalid JSON")
        self.table_path.write_text("stale table")
        with self.assertRaises(json.JSONDecodeError):
            self.render_file()
        self.assertFalse(self.table_path.exists())

    def test_changed_seeds_change_caption_and_run_counts(self):
        table = render.render_table(report())
        self.assertIn("Ranges span seeds 8 and 43", table)
        self.assertNotIn("7, 42, and 12345", table)
        self.assertEqual(table.count("2/2"), 6)
        one = render.render_table(report((99,)))
        self.assertIn("Seed: 99", one)
        self.assertEqual(one.count("1/1"), 7)

    def test_duplicate_or_missing_seed_observations_are_rejected(self):
        original = report()
        for mutate in (lambda d: d["observations"].pop(),
                       lambda d: d["observations"][2].update(seed=8),
                       lambda d: d["method"].update(seeds=[0, 43])):
            data = copy.deepcopy(original)
            mutate(data)
            with self.assertRaises(ValueError):
                render.render_table(data)

    def test_historical_schema_one_report_is_supported(self):
        data = json.loads((ROOT / "paper/results/current_eval.json").read_text())
        original = ROOT / "paper/results/provenance/evaluate-v1.py.txt"
        self.assertEqual(hashlib.sha256(original.read_bytes()).hexdigest(),
                         data["input_sha256"]["paper/evaluate.py"])
        table = render.render_table(data)
        self.assertIn("7, 42, and 12345", table)
        self.assertIn("Startup abort", table)
        self.assertEqual(table.count("3/3"), 3)

    def test_failed_baseline_is_rejected(self):
        data = report()
        data["observations"][0]["differential_passed"] = False
        with self.assertRaisesRegex(ValueError, "failed baseline"):
            render.render_table(data)

    def test_input_cannot_be_overwritten_by_table(self):
        self.json_path.write_text(json.dumps(report()))
        before = self.json_path.read_bytes()
        with patch.object(sys, "argv", ["render_results.py", "--input", str(self.json_path),
                                       "--output", str(self.json_path)]):
            with self.assertRaises(SystemExit):
                render.main()
        self.assertEqual(self.json_path.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
