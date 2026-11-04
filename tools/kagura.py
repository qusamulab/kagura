#!/usr/bin/env python3
"""Compile native C/C++/ObjC through Kagura on LLVM 17+.

Kagura flags are passed directly: kagura -kagura-str -O2 main.c -o app
Use --clang/--kagura-opt/--plugin/--runtime to select an explicit toolchain.
The driver deliberately rejects unsupported compilation modes before writing.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


class DriverError(Exception):
    pass


class ToolError(Exception):
    def __init__(self, status):
        self.status = status if status > 0 else 128 - status


def run(args, verbose=False, **kwargs):
    if verbose:
        print("[kagura] " + repr([str(a) for a in args]), file=sys.stderr)
    result = subprocess.run([str(a) for a in args], **kwargs)
    if result.returncode:
        raise ToolError(result.returncode)
    return result


def executable(value, default):
    found = shutil.which(value or default)
    if not found:
        raise DriverError(f"Cannot find {value or default}; select the matching LLVM toolchain.")
    return found


SOURCE_LANG = {".c": "c", ".cc": "c++", ".cpp": "c++", ".cxx": "c++",
               ".C": "c++", ".m": "objective-c", ".mm": "objective-c++",
               ".ll": "ir", ".bc": "ir"}
KAGURA_VALUES = {"config", "seed", "build-id", "protect", "deny", "allow",
                 "dwarf", "symmap-out", "audit-out", "bcf-prob", "bcf-iter",
                 "sub-iter", "dci-prob"}
PAIRED = {"-I", "-isystem", "-iquote", "-idirafter", "-include", "-imacros",
          "-D", "-U", "-isysroot", "--sysroot", "-target", "--target", "-arch",
          "-B", "-Xclang", "-Xassembler", "-Xlinker", "-L", "-l", "-F",
          "-framework", "-iframework", "-MF", "-MT", "-MQ", "-mllvm",
          "-include-pch", "-resource-dir", "-gcc-toolchain", "--gcc-toolchain"}
LINK_PAIRS = {"-Xlinker", "-L", "-l", "-framework"}
PREPROCESS_PAIRS = {"-I", "-isystem", "-iquote", "-idirafter", "-include",
                    "-imacros", "-D", "-U", "-MF", "-MT", "-MQ", "-include-pch"}


def runtime_libraries(directory, selections, args):
    runtime = selections.get("--runtime") or os.environ.get("KAGURA_RUNTIME")
    cross = any(a in {"-target", "--target", "-arch", "-isysroot", "--sysroot"} or
                a.startswith(("--target=", "--sysroot=")) for a in args)
    if not runtime and not cross:
        for candidate in (directory.parent / "lib/libkagura_runtime.a",
                          directory.parent / "lib/kagura_runtime.lib",
                          directory.parent / "runtime/libkagura_runtime.a",
                          directory.parent / "runtime/kagura_runtime.lib"):
            if candidate.is_file():
                runtime = str(candidate)
                break
    libraries = [runtime] if runtime else []
    if runtime and not cross:
        libraries += ["-lws2_32", "-limagehlp", "-lbcrypt"] if os.name == "nt" else (
            ["-ldl"] if sys.platform.startswith("linux") else [])
    # clang's GNU-style MSVC linker driver still adds libcmt explicitly for
    # object-only links, even with -fms-runtime-lib=dll. Override that default
    # to match the frontend's _DLL declarations and .drectve metadata.
    if os.name == "nt":
        mode = next((a.partition("=")[2] for a in reversed(args)
                     if a.startswith("-fms-runtime-lib=")), "static")
        crt = {"dll": "msvcrt", "dll_dbg": "msvcrtd", "static_dbg": "libcmtd"}.get(mode)
        if crt:
            libraries += ["-Xlinker", "/nodefaultlib:libcmt", "-Xlinker", "/defaultlib:" + crt]
    return libraries


def main(args):
    if not args or args == ["--help"]:
        print(__doc__)
        print("--config-check FILE validates a policy without compiling source.\n"
              "--verbose prints commands. Python 3.9+ and matching clang/kagura-opt are required.")
        return 0
    directory = Path(__file__).resolve().parent
    selections, forwarded, protection = {}, [], []
    verbose = False
    i = 0
    while i < len(args):
        arg = args[i]
        key, sep, inline = arg.partition("=")
        if key in {"--clang", "--kagura-opt", "--plugin", "--runtime", "--config-check"}:
            if sep:
                value = inline
            else:
                i += 1
                if i == len(args):
                    raise DriverError(f"{key} requires a value")
                value = args[i]
            selections[key] = value
        elif arg == "--verbose":
            verbose = True
        elif key.startswith("-kagura-"):
            protection.append(arg)
            if not sep and key[len("-kagura-"):] in KAGURA_VALUES:
                i += 1
                if i == len(args):
                    raise DriverError(f"{key} requires a value")
                protection[-1] += "=" + args[i]
        elif arg.startswith("@") or arg.startswith(("-flto", "-fpass-plugin", "-save-temps")):
            raise DriverError(f"Unsupported driver option {arg}; use explicit per-source compilation.")
        else:
            forwarded.append(arg)
        i += 1

    opt = executable(selections.get("--kagura-opt") or os.environ.get("KAGURA_OPT"),
                     str(directory / ("kagura-opt.exe" if os.name == "nt" else "kagura-opt")))
    plugin = ["-load-kagura=" + selections["--plugin"]] if "--plugin" in selections else []
    if "--config-check" in selections:
        if forwarded or protection:
            raise DriverError("--config-check accepts a policy path and tool-selection options only")
        run([opt, *plugin, "-kagura-config-check",
             "-kagura-config=" + selections["--config-check"]], verbose)
        return 0

    clang_default = str(Path(os.environ["LLVM_PREFIX"]) / "bin/clang") if os.environ.get("LLVM_PREFIX") else "clang"
    clang = executable(selections.get("--clang") or os.environ.get("KAGURA_CLANG"), clang_default)
    if any(a in {"-E", "-M", "-MM", "-fsyntax-only", "--version", "-dumpmachine", "-print-resource-dir"}
           for a in forwarded):
        run([clang, *forwarded], verbose)
        return 0

    compile_args, backend_args, link_args, inputs = [], [], [], []
    language, output, mode, emit_ir, level = None, None, "link", False, "2"
    deps = False
    i = 0
    while i < len(forwarded):
        arg = forwarded[i]
        if arg in {"-o", "-x"}:
            i += 1
            if i == len(forwarded):
                raise DriverError(f"{arg} requires a value")
            if arg == "-o":
                output = forwarded[i]
            else:
                language = None if forwarded[i] == "none" else forwarded[i]
        elif arg in {"-c", "-S"}:
            mode = arg
        elif arg == "-emit-llvm":
            emit_ir = True
        elif arg in PAIRED:
            i += 1
            if i == len(forwarded):
                raise DriverError(f"{arg} requires a value")
            pair = [arg, forwarded[i]]
            if arg == "-mllvm":
                raise DriverError("Pass -kagura-* options directly, without -mllvm")
            if arg in LINK_PAIRS:
                link_args.extend(pair)
            elif arg in PREPROCESS_PAIRS:
                compile_args.extend(pair)
            else:
                compile_args.extend(pair)
                backend_args.extend(pair)
                link_args.extend(pair)
        elif arg in {"-MD", "-MMD", "-MP", "-MG"}:
            compile_args.append(arg)
            deps |= arg in {"-MD", "-MMD"}
        elif arg.startswith(("-Wl,", "-L", "-l", "-fuse-ld=")) or arg in {
                "-shared", "-static", "-rdynamic", "-pie", "-no-pie", "-s"}:
            link_args.append(arg)
        elif arg.startswith(("-I", "-D", "-U", "-std=", "-isystem", "-iquote")):
            compile_args.append(arg)
        elif arg.startswith("-"):
            if arg in {"-###", "--", "-MJ", "-arch_multiple"} or arg.startswith("-o"):
                raise DriverError(f"Unsupported driver option {arg}")
            if arg.startswith("-O"):
                if arg[2:] not in {"0", "1", "2", "3", "s", "z", "fast", "g"}:
                    raise DriverError(f"Unsupported optimization level {arg}")
                level = {"fast": "3", "g": "1"}.get(arg[2:], arg[2:])
            compile_args.append(arg)
            backend_args.append(arg)
            link_args.append(arg)
        else:
            lang = language or SOURCE_LANG.get(Path(arg).suffix)
            if lang:
                if lang not in {*SOURCE_LANG.values()}:
                    raise DriverError(f"Unsupported source language {lang}")
                inputs.append((arg, lang))
                link_args.append((arg, lang))
            else:
                link_args.append(arg)
        i += 1
    if not inputs:
        run([clang, *forwarded, *runtime_libraries(directory, selections, forwarded)], verbose)
        return 0
    if emit_ir and mode == "link":
        raise DriverError("-emit-llvm requires -c or -S")
    if output and len(inputs) > 1 and mode != "link":
        raise DriverError("One -o output cannot be used for multiple source files with -c/-S")
    if sum(a == "-arch" for a in compile_args) > 1:
        raise DriverError("Compile each -arch separately before combining universal binaries")

    if not any(a.startswith("-O") for a in compile_args):
        compile_args.append("-O2")
        backend_args.append("-O2")

    version = run([opt, *plugin, "--version"], capture_output=True, text=True).stdout
    compiler_version = run([clang, "--version"], capture_output=True, text=True).stdout
    llvm = re.search(r"LLVM version (\d+)", version)
    cc = re.search(r"clang version (\d+)", compiler_version)
    if (not llvm or not cc or llvm[1] != cc[1] or
            not 17 <= int(llvm[1]) <= 22 or "Apple clang" in compiler_version):
        raise DriverError("clang and kagura-opt must use the same supported upstream LLVM major (17–22); use --clang")

    for arg in protection:
        if arg.startswith("-kagura-config="):
            run([opt, *plugin, "-kagura-config-check", arg], verbose)
    if any(lang in {"c++", "objective-c++"} for _, lang in inputs):
        cxx = ["--driver-mode=g++"]
    else:
        cxx = []
    with tempfile.TemporaryDirectory(prefix="kagura-") as temporary:
        compiled = {}
        for number, (source, lang) in enumerate(inputs):
            before = Path(temporary) / f"{number}.{Path(source).name}.bc"
            after = Path(temporary) / f"{number}.protected.bc"
            destination = output or str(Path(source).with_suffix("").name) + (
                ".ll" if emit_ir and mode == "-S" else ".bc" if emit_ir else
                ".s" if mode == "-S" else ".obj" if os.name == "nt" else ".o")
            if lang == "ir":
                before = Path(source)
            else:
                dependency_args = []
                if deps:
                    if "-MF" not in compile_args:
                        dependency_args += ["-MF", str(Path(destination).with_suffix(".d"))]
                    if "-MT" not in compile_args and "-MQ" not in compile_args:
                        dependency_args += ["-MT", destination]
                run([clang, *compile_args, *dependency_args, "-x", lang,
                     "-Xclang", "-disable-llvm-passes", "-emit-llvm", "-c", source,
                     "-o", before], verbose)
            run([opt, *plugin, "-O" + level, *protection,
                 *(["-S"] if emit_ir and mode == "-S" else []), before, "-o", after], verbose)
            if emit_ir:
                if destination == "-":
                    sys.stdout.buffer.write(after.read_bytes())
                else:
                    shutil.copyfile(after, destination)
                continue
            obj = Path(temporary) / f"{number}.o" if mode == "link" else destination
            run([clang, *backend_args, "-x", "ir", "-Xclang", "-disable-llvm-passes",
                 "-S" if mode == "-S" else "-c", after, "-o", obj], verbose)
            compiled[(source, lang)] = str(obj)
        if mode == "link":
            libraries = runtime_libraries(directory, selections, compile_args)
            run([clang, *cxx, *(compiled.get(a, a) if isinstance(a, tuple) else a for a in link_args),
                 *libraries, *(["-o", output] if output else [])], verbose)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except DriverError as error:
        print(f"kagura: {error}", file=sys.stderr)
        sys.exit(2)
    except ToolError as error:
        sys.exit(error.status)
    except OSError as error:
        print(f"kagura: {error}", file=sys.stderr)
        sys.exit(1)
