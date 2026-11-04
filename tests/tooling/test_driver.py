"""Real compiler-driver and check-only policy tests, used in both linkage modes."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--driver", required=True)
parser.add_argument("--clang", required=True)
parser.add_argument("--kagura-opt", required=True)
parser.add_argument("--runtime", required=True)
options, remaining = parser.parse_known_args()


class DriverTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="kagura-driver-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def invoke(self, *args, success=True):
        result = subprocess.run([sys.executable, options.driver, "--clang", options.clang,
                                 "--kagura-opt", options.kagura_opt, "--runtime", options.runtime,
                                 *(["-fms-runtime-lib=dll"] if os.name == "nt" and
                                   "--config-check" not in args else []), *map(str, args)], cwd=self.root, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def test_check_only_validates_all_policy_layers_without_source(self):
        for policy, valid in (({"profile": "CUSTOM", "string": {"scoped": True}}, True),
                              ({"passes": {"str": "yes"}}, False),
                              ({"tuning": {"bcf_prob": 101}}, False),
                              ({"tuning": {"bcf_iter": -1}}, False),
                              ({"unknown": True}, False),
                              ({"flavors": {"unused": {"passes": {"str": 1}}}}, False),
                              ({"passes": {"anti-debug": True, "anti_debug": False}}, False)):
            with self.subTest(policy=policy):
                path = self.write("policy.json", json.dumps(policy))
                self.invoke("--config-check", path, success=valid)
        self.invoke("--config-check", self.write("bad.json", "{bad"), success=False)
        self.invoke("--config-check", "missing.json", success=False)
        self.assertFalse(list(self.root.glob("*.bc")))

    def test_sibling_tool_and_runtime_discovery(self):
        self.write("main.c", '#include <stdio.h>\nint main(void){puts("discovered runtime");return 0;}\n')
        result = subprocess.run([sys.executable, options.driver, "--clang", options.clang,
                                 *(["-fms-runtime-lib=dll"] if os.name == "nt" else []),
                                 "-O1", "-kagura-str-aes", "main.c", "-o", "auto-program"],
                                cwd=self.root, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        run = subprocess.run([str(self.root / "auto-program")], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout, "discovered runtime\n")

    def test_multiple_sources_headers_spaces_and_defines(self):
        self.write("headers with spaces/value.h", "#define EXPECTED VALUE\n")
        self.write("value.c", '#include "value.h"\nint value(void){return EXPECTED;}\n')
        self.write("main.c", '#include <stdio.h>\nint value(void);\nint main(void){puts("hello driver");return value()!=42;}\n')
        self.invoke("-O1", "-Werror", "-I", "headers with spaces", "-DVALUE=42",
                    "-kagura-str", "main.c", "value.c", "-o", "program")
        result = subprocess.run([str(self.root / "program")], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "hello driver\n")

    def test_cpp_standard_library(self):
        self.write("main.cpp", "#include <vector>\nint main(){std::vector<int> a{1,2,3}; return a.size()!=3;}\n")
        self.invoke("-O1", "-std=c++17", "main.cpp", "-o", "cpp-program")
        self.assertEqual(subprocess.run([str(self.root / "cpp-program")], timeout=10).returncode, 0)

    def test_scoped_plaintext_is_wiped_in_emitted_ir(self):
        self.write("main.c", '#include <stdio.h>\nint main(void){return puts("scoped test secret")<0;}\n')
        for flag in ("-kagura-str", "-kagura-str-aes"):
            with self.subTest(flag=flag):
                self.invoke("-O1", flag, "-kagura-string-scoped", "-S", "-emit-llvm",
                            "main.c", "-o", "protected.ll")
                text = (self.root / "protected.ll").read_text()
                self.assertIn("kagura.scoped", text)
                self.assertIn("@llvm.memset", text)
                self.assertIn("i1 true", text)
                self.assertNotIn('c"scoped test secret', text)

    def test_scoped_runtime_and_separate_link_command(self):
        self.write("main.c", '#include <stdio.h>\nint main(void){for(int i=0;i<20;++i)puts("repeat scoped secret");return 0;}\n')
        for flag in ("-kagura-str", "-kagura-str-aes"):
            with self.subTest(flag=flag):
                self.invoke("-O1", flag, "-kagura-string-scoped", "-c", "main.c", "-o", "main.o")
                self.invoke("main.o", "-o", "program")
                result = subprocess.run([str(self.root / "program")], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "repeat scoped secret\n" * 20)

    def test_scoped_buffers_are_private_to_threads_and_recursive_calls(self):
        self.write("scoped.ll", '@secret = private unnamed_addr constant [19 x i8] c"concurrent secret!\\00"\ndeclare i32 @consume(ptr nocapture readonly) nounwind\ndefine i32 @scoped_value() noinline {\n  %r = call i32 @consume(ptr @secret)\n  ret i32 %r\n}\n')
        self.write("main.cpp", '#include <thread>\n#include <vector>\n#include <cstring>\n#include <atomic>\nextern "C" int scoped_value();\nstatic thread_local int depth;\nstatic std::atomic<int> failures{0};\nextern "C" int consume(const char *s) {\n    if (std::strcmp(s, "concurrent secret!") != 0) ++failures;\n    if (++depth < 3) scoped_value();\n    --depth;\n    if (std::strcmp(s, "concurrent secret!") != 0) ++failures;\n    return 0;\n}\nint main() {\n    std::vector<std::thread> threads;\n    for (int t=0;t<6;++t) threads.emplace_back([]{\n        for (int i=0;i<100;++i) scoped_value();\n    });\n    for (auto &t:threads) t.join();\n    return failures.load() != 0;\n}\n')
        for flag in ("-kagura-str", "-kagura-str-aes"):
            with self.subTest(flag=flag):
                self.invoke("-O1", "-std=c++17", "-pthread", flag, "-kagura-string-scoped",
                            "scoped.ll", "main.cpp", "-o", "threaded-program")
                result = subprocess.run([str(self.root / "threaded-program")],
                                        capture_output=True, text=True, timeout=20)
                self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(os.name == "nt", "Windows DLL-import execution")
    def test_windows_call_indirection_keeps_loader_dependencies(self):
        self.write("main.c", '#include <windows.h>\n#include <stdio.h>\nint main(void){puts("windows indirection");return GetCurrentProcessId()==0;}\n')
        self.invoke("-O1", "-kagura-ci", "-kagura-anti-debug", "main.c", "-o", "indirect-program")
        result = subprocess.run([str(self.root / "indirect-program")],
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "windows indirection\n")

    def test_cross_target_ir_keeps_target_triple(self):
        self.write("source.c", "int value(void){return 42;}\n")
        self.invoke("--target=wasm32", "-O1", "-S", "-emit-llvm", "source.c", "-o", "wasm.ll")
        self.assertIn('target triple = "wasm32', (self.root / "wasm.ll").read_text())

    def test_dependency_target_is_final_object(self):
        self.write("source.c", "int value(void){return 42;}\n")
        self.invoke("-O1", "-MMD", "-c", "source.c", "-o", "final.o")
        self.assertIn("final.o:", (self.root / "final.d").read_text())

    def test_source_and_link_failures_are_not_hidden(self):
        self.invoke("missing.c", "-o", "bad-output", success=False)
        self.write("invalid.c", "this is not C;\n")
        self.invoke("invalid.c", "-o", "bad-output", success=False)
        self.write("undefined.c", "void missing(void); int main(void){missing();}\n")
        self.invoke("undefined.c", "-o", "bad-output", success=False)
        self.assertFalse((self.root / "bad-output").exists())

    def test_unsupported_modes_fail_before_producing_output(self):
        self.write("source.c", "int main(void){return 0;}\n")
        for args in (("-flto",), ("@response.txt",), ("-mllvm", "-kagura-str")):
            self.invoke(*args, "source.c", "-o", "bad-output", success=False)
        self.assertFalse((self.root / "bad-output").exists())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
