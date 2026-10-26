#!/usr/bin/env python3
"""Cross-check every registered pass against the standalone lit IR corpus.

This checks validity and serialization, not semantic equivalence. Positive
transformation assertions and native execution remain in lit and CTest.
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--opt', required=True)
    parser.add_argument('--plugin', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seeds', type=int, nargs='+', default=[32, 42])
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    args.output.mkdir(parents=True, exist_ok=True)
    try:
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    except ImportError:
        pass
    registry = (root / 'include/kagura/PassRegistry.def').read_text()
    passes = re.findall(
        r'^KAGURA_(?:FN|MOD|INFRA)_PASS\([^\n]*?"(kagura-[^"]+)"',
        registry, re.M)
    inputs = sorted((root / 'tests/lit').glob('*.ll'))
    # Most named passes run without enable flags. These infrastructure passes
    # require an explicit mode; policy and reporting modes have their own tests.
    modes = {'kagura-vtp': ['-kagura-vtp'],
             'kagura-dwarf-control': ['-kagura-dwarf=obfuscate'],
             'kagura-bcf': ['-kagura-bcf-prob=100'],
             'kagura-dci': ['-kagura-dci-prob=100']}

    def run(item):
        name, source, seed = item
        command = [args.opt, '-load-pass-plugin=' + args.plugin,
                   '-kagura-seed=' + str(seed), '-passes=' + name + ',verify',
                   '-S', str(source), '-o', '-'] + modes.get(name, [])
        try:
            result = subprocess.run(command, text=True, capture_output=True,
                                    timeout=20)
            ir = result.stdout
            if result.returncode == 0:
                # The verifier alone misses malformed constant expressions in
                # some non-assertions LLVM builds. Reparse the printed output.
                result = subprocess.run(
                    [args.opt, '-passes=verify', '-disable-output', '-'],
                    input=ir, text=True, capture_output=True, timeout=20)
            if result.returncode == 0:
                return None
            error = result.stderr
        except subprocess.TimeoutExpired:
            ir, error = '', 'Timed out after 20 seconds'
        stem = f'{name}--{source.stem}--{seed}'
        (args.output / (stem + '.ll')).write_text(ir)
        (args.output / (stem + '.err')).write_text(error)
        return {'pass': name, 'input': source.name, 'seed': seed,
                'diagnostic': error.split('PLEASE')[0][:1000]}

    items = [(name, source, seed) for name in passes for source in inputs
             for seed in args.seeds]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        failures = [result for result in pool.map(run, items) if result]
    report = {'runs': len(items), 'passes': passes, 'seeds': args.seeds,
              'inputs': [str(p.relative_to(root)) for p in inputs],
              'failures': failures}
    (args.output / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(items)} checks; {len(failures)} failures')
    for failure in failures:
        print(json.dumps(failure))
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
