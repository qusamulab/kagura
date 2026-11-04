"""Policy correctness, scoped inspection, CLI compatibility and real stdio MCP."""

import asyncio
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from kagura_mcp.binary import BinaryInfo, structured_report
from kagura_mcp.inspection import Workspace, doctor
from kagura_mcp.policy import catalog, generate_config, load_config, validate_config

REPO = Path(__file__).resolve().parents[2]


class PolicyTests(unittest.TestCase):
    def test_profiles_match_shipped_integration_files(self):
        for name, preset in catalog()["profiles"].items():
            shipped = json.loads((REPO / "integration/profiles" / f"{name.lower()}.json").read_text())
            self.assertEqual(preset, {k: shipped[k] for k in ("passes", "tuning")})
            self.assertEqual(generate_config(name)["config"]["passes"], shipped["passes"])
            self.assertTrue(validate_config(shipped)["valid"])

    def test_current_opt_in_passes_are_available(self):
        passes = {key: True for key in ("string-split", "cse-break", "bbcheck", "elt", "telemetry")}
        cfg = generate_config("CUSTOM", passes=passes)["config"]
        self.assertTrue(cfg["passes"]["cse_break"])
        self.assertNotIn("tuning", cfg)

    def test_explicit_false_overrides_profile(self):
        cfg = generate_config("balanced", passes={"anti-debug": False}, tuning={"seed": 0})["config"]
        self.assertFalse(cfg["passes"]["anti_debug"])
        self.assertEqual(cfg["tuning"]["seed"], 0)

    def test_cli_only_and_unknown_root_keys_are_errors(self):
        for cfg in ({"audit": True}, {"build_id": "a"}, {"seed": 12}, {"allow": ["foo"]},
                    {"passes": {"vtp": True}}, {"tuning": {"typo": 12}}):
            with self.subTest(config=cfg):
                self.assertFalse(validate_config(cfg)["valid"])
        self.assertIn("-kagura-audit", validate_config({"audit": True})["errors"][0]["message"])

    def test_wrong_types_and_ranges_fail(self):
        for cfg in ([], {"passes": []}, {"passes": {"str": 1}}, {"passes": {"dwarf": "bad"}},
                    {"tuning": {"seed": True}}, {"tuning": {"seed": -1}},
                    {"tuning": {"seed": 2**63}}, {"tuning": {"bcf_prob": 101}},
                    {"tuning": {"bcf_iter": 2**32}}, {"string": {"aggressive": 1}},
                    {"profile": "TYPO"}, {"denylist": "test_*"}, {"protect": ["a,b"]}):
            with self.subTest(config=cfg):
                self.assertFalse(validate_config(cfg)["valid"])

    def test_valid_flavors_and_dwarf(self):
        cfg = {"profile": "custom", "passes": {"dwarf": "strip", "str-aes": True},
               "flavors": {"release": {"tuning": {"seed": 42}, "string": {"aggressive": True}}},
               "audit_out": "logs/audit.json"}
        self.assertTrue(validate_config(cfg)["valid"])

    def test_invalid_unused_flavors_are_checked(self):
        for flavor in (False, {"flavors": {}}, {"audit_out": "a"}, {"passes": {"bogus": True}}):
            self.assertFalse(validate_config({"flavors": {"unused": flavor}})["valid"])

    def test_duplicate_alias_and_literal_keys_rejected(self):
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            load_config('{"passes":{},"passes":{}}')
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            generate_config(passes={"str-aes": True, "str_aes": False})
        self.assertFalse(validate_config({"tuning": {"bcf-prob": 1, "bcf_prob": 2}})["valid"])

    def test_non_json_constants_rejected(self):
        with self.assertRaises(ValueError):
            load_config('{"seed":NaN}')

    def test_scoped_policy_and_flavor_validation(self):
        result = generate_config("CUSTOM", passes={"str_aes": True},
                                 aggressive_strings=True, scoped_strings=True)
        self.assertEqual(result["config"]["string"], {"aggressive": True, "scoped": True})
        self.assertTrue(validate_config(result["config"])["valid"])
        self.assertTrue(any("persistent" in note for note in result["notes"]))
        self.assertTrue(validate_config({"flavors": {"release": {"string": {"scoped": False}}}})["valid"])
        for value in (1, "true", None, []):
            self.assertFalse(validate_config({"string": {"scoped": value}})["valid"])
        self.assertFalse(validate_config({"string": {"scope": True}})["valid"])

    def test_invalid_generation_never_produces_policy(self):
        with self.assertRaises(ValueError):
            generate_config("TYPO")
        with self.assertRaises(ValueError):
            generate_config(passes={"typo": True})


class WorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.workspace = Workspace(self.root)

    def test_doctor_requires_matching_supported_toolchain_and_driver(self):
        directory = self.root / "build/bin"
        directory.mkdir(parents=True)
        for name in ("clang", "kagura-opt", "kagura"):
            (directory / name).touch()

        def version(args, **kwargs):
            compiler = Path(args[0]).name == "clang"
            output = clang_version if compiler else "LLVM version 21.1.0"
            return subprocess.CompletedProcess(args, 0, output, "")

        with patch("kagura_mcp.inspection.shutil.which", return_value=None), \
                patch("kagura_mcp.inspection.subprocess.run", side_effect=version):
            for clang_version, expected in (("clang version 21.1.0", True),
                                            ("clang version 22.0.0", False),
                                            ("Apple clang version 21.0.0", False),
                                            ("unrecognized", False)):
                result = doctor(self.workspace, llvm_bin=directory)
                self.assertEqual(result["compiler_driver_ready"], expected, result)
            (directory / "kagura").unlink()
            clang_version = "clang version 21.1.0"
            self.assertFalse(doctor(self.workspace, llvm_bin=directory)["compiler_driver_ready"])

    def test_paths_cannot_escape(self):
        with tempfile.TemporaryDirectory() as other:
            secret = Path(other) / "outside.json"
            secret.write_text("{}")
            with self.assertRaisesRegex(ValueError, "inside"):
                self.workspace.file(str(secret))
            if os.name != "nt":
                (self.root / "link.json").symlink_to(secret)
                with self.assertRaisesRegex(ValueError, "inside"):
                    self.workspace.file("link.json")

    def test_size_limit_and_directory_rejected(self):
        (self.root / "large.json").write_text("12345")
        with self.assertRaisesRegex(ValueError, "limit"):
            self.workspace.file("large.json", 4)
        with self.assertRaisesRegex(ValueError, "regular"):
            self.workspace.file(".")

    def test_file_validation_reports_parse_errors(self):
        (self.root / "broken.json").write_text('{"passes":')
        self.assertFalse(self.workspace.validate("broken.json")["valid"])

    def test_jsonl_pagination_filters_and_malformed_records(self):
        records = [
            {"module": "payment.c", "functions": [{"name": "pay", "passes": ["str", "fla"]},
                                                     {"name": "verify", "passes": ["str"]}]},
            {"module": "ui.c", "functions": [{"name": "draw", "passes": ["str"]}]},
        ]
        (self.root / "audit.json").write_text("\n".join(json.dumps(r) for r in records) +
                                               '\n{broken\n{"module":"bad","functions":[null]}\n')
        first = self.workspace.audit("audit.json", module="payment", limit=1)
        self.assertFalse(first["complete"])
        self.assertEqual(first["error_count"], 2)
        self.assertEqual([e["line"] for e in first["errors"]], [3, 4])
        self.assertEqual(first["pass_counts"], {"str": 2, "fla": 1})
        self.assertEqual(first["next_offset"], 1)
        second = self.workspace.audit("audit.json", module="payment", offset=1, limit=1)
        self.assertEqual(second["functions"][0]["name"], "verify")
        self.assertIsNone(second["next_offset"])
        filtered = self.workspace.audit("audit.json", function="draw")
        self.assertEqual(filtered["function_entries"], 1)

    def test_audit_error_details_are_bounded(self):
        (self.root / "audit.json").write_text("null\n" * 80)
        result = self.workspace.audit("audit.json")
        self.assertEqual(result["error_count"], 80)
        self.assertEqual(len(result["errors"]), 50)

    def test_invalid_pagination_rejected(self):
        for options in ({"offset": -1}, {"limit": 0}, {"limit": 501}):
            with self.assertRaises(ValueError):
                self.workspace.audit("missing", **options)

    def test_structured_binary_metrics_and_bounded_samples(self):
        a = BinaryInfo("a", 10, {"text": 8}, {"a", "b"}, {"a"}, ["hello"])
        b = BinaryInfo("b", 15, {"text": 12}, {"c"}, set(), [])
        result = structured_report(a, b, limit=1)
        self.assertEqual(result["metrics"]["size_bytes"]["percent"], 50)
        self.assertEqual(result["removed_symbols"], {"count": 2, "sample": ["a"]})

    @unittest.skipIf(os.name == "nt", "Native POSIX tool integration")
    def test_native_binary_comparison(self):
        compiler = shutil.which("clang") or shutil.which("cc")
        if not compiler or not all(shutil.which(x) for x in ("nm", "size", "strings")):
            self.skipTest("Compiler and binary inspection tools not installed")
        (self.root / "main.c").write_text('int main(void) { return 0; }\n')
        subprocess.run([compiler, str(self.root / "main.c"), "-o", str(self.root / "app")], check=True,
                       capture_output=True, timeout=30)
        report = self.workspace.compare("app", "app")
        self.assertEqual(report["metrics"]["size_bytes"]["delta"], 0)
        self.assertEqual(report["added_symbols"]["count"], 0)


class CLITests(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(REPO / "scripts/cli/kagura-cli.py"), *args],
                              capture_output=True, text=True, timeout=10)

    def test_config_gen_emits_only_effective_policy_settings(self):
        result = self.run_cli("config-gen", "--profile", "FAST", "--seed", "0", "--allow", "pay*",
                              "--deny", "test_*", "--audit", "--audit-out", "audit.json",
                              "--symmap", "--symmap-out", "symbols.json", "--build-id", "release",
                              "--dwarf", "strip")
        self.assertEqual(result.returncode, 0, result.stderr)
        config = json.loads(result.stdout)
        self.assertTrue(validate_config(config)["valid"])
        self.assertTrue(config["passes"]["anti_debug"])
        self.assertEqual(config["allowlist"], ["pay*"])
        self.assertEqual(config["denylist"], ["test_*"])
        self.assertEqual(config["tuning"]["seed"], 0)
        for flag in ("-kagura-audit", "-kagura-symmap", "-kagura-build-id=release"):
            self.assertIn(flag, result.stderr)

    def test_cli_generates_both_string_modes(self):
        result = self.run_cli("config-gen", "--profile", "CUSTOM", "--passes", "str-aes",
                              "--aggressive-strings", "--scoped-strings")
        self.assertEqual(result.returncode, 0, result.stderr)
        config = json.loads(result.stdout)
        self.assertEqual(config["string"], {"aggressive": True, "scoped": True})
        self.assertTrue(validate_config(config)["valid"])

    def test_unknown_pass_does_not_write_output(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "policy.json"
            result = self.run_cli("config-gen", "--passes", "typo", "-o", str(output))
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(output.exists())

    def test_check_exit_codes_without_llvm(self):
        with tempfile.TemporaryDirectory() as temp:
            policy = Path(temp) / "policy.json"
            for text, code in (('{"profile":"FAST"}', 0), ('{"audit":true}', 1), ('{bad', 1)):
                policy.write_text(text)
                result = self.run_cli("config-check", str(policy))
                self.assertEqual(result.returncode, code, result.stderr)
                self.assertEqual(json.loads(result.stdout)["valid"], code == 0)


class MCPStdioTests(unittest.IsolatedAsyncioTestCase):
    async def test_real_client_discovers_and_calls_tools(self):
        from mcp import ClientSession, StdioServerParameters
        from mcp.client.stdio import stdio_client

        with tempfile.TemporaryDirectory() as temp:
            (Path(temp) / "policy.json").write_text('{"profile":"FAST"}')
            (Path(temp) / "audit.json").write_text(json.dumps({
                "module": "payment.c", "functions": [{"name": "pay", "passes": ["str"]}]
            }) + "\n")
            parameters = StdioServerParameters(command=sys.executable, args=[
                "-m", "kagura_mcp.server", "--workspace", temp,
            ])
            async with stdio_client(parameters) as (read, write):
                async with ClientSession(read, write) as session:
                    await asyncio.wait_for(session.initialize(), 20)
                    listed = await session.list_tools()
                    self.assertEqual({tool.name for tool in listed.tools}, {
                        "doctor", "list_passes", "generate_config", "validate_config",
                        "inspect_audit", "compare_binaries",
                    })
                    generation = next(t for t in listed.tools if t.name == "generate_config")
                    self.assertIn("scoped_strings", generation.inputSchema["properties"])
                    self.assertTrue(all(tool.annotations.readOnlyHint for tool in listed.tools))
                    for name, args in (
                        ("doctor", {}),
                        ("list_passes", {}),
                        ("generate_config", {"profile": "FAST", "passes": {"anti_debug": False}}),
                        ("validate_config", {"path": "policy.json"}),
                        ("inspect_audit", {"path": "audit.json"}),
                    ):
                        response = await session.call_tool(name, args)
                        self.assertFalse(response.isError, response)
                        self.assertIsInstance(response.structuredContent, dict)
                    scoped = await session.call_tool("generate_config", {"profile": "CUSTOM",
                        "passes": {"str_aes": True}, "scoped_strings": True})
                    self.assertTrue(scoped.structuredContent["config"]["string"]["scoped"])
                    invalid = await session.call_tool("validate_config", {"config": {"passes": {"typo": True}}})
                    self.assertFalse(invalid.structuredContent["valid"])
                    rejected = await session.call_tool("validate_config", {})
                    self.assertTrue(rejected.isError)
                    missing = await session.call_tool("compare_binaries", {"baseline": "missing", "obfuscated": "missing"})
                    self.assertTrue(missing.isError)


if __name__ == "__main__":
    unittest.main()
