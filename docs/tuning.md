# Tuning Parameters

> **LLVM 22 required for `-mllvm` flags.** On LLVM 17–21, clang parses `-mllvm`
> options before `-fpass-plugin` has loaded the plugin, so every `-kagura-*`
> flag is rejected with *"Unknown command line argument"*. Use the shipped
> `kagura-opt`, or `opt --load-pass-plugin=<plugin> -kagura-… -passes=…`, both
> of which work on all supported versions. See
> [Known issues](https://github.com/qusamulab/kagura/blob/main/docs/project/CHANGELOG.md#known-issues).


Every flag below can be set via `-mllvm -kagura-<flag>=<value>` on the clang
command line. Only some of them have a [JSON policy file](configuration.md)
equivalent — see [What is settable from a policy
file](#what-is-settable-from-a-policy-file).

## Core tuning

These five, and only these five, are the `"tuning"` object.

| Option | JSON key | Default | Description |
|:-------|:---------|:--------|:------------|
| `-kagura-seed=<N>` | `seed` | `0` (fixed default) | PRNG seed for reproducible output |
| `-kagura-bcf-prob=<N>` | `bcf_prob` | `30` | Bogus CF probability per BB [0-100] |
| `-kagura-bcf-iter=<N>` | `bcf_iter` | `1` | Bogus CF iterations |
| `-kagura-sub-iter=<N>` | `sub_iter` | `1` | Substitution iterations |
| `-kagura-dci-prob=<N>` | `dci_prob` | `40` | Dead code insertion probability [0-100] |

## Infrastructure

| Option | JSON key | Default | Description |
|:-------|:---------|:--------|:------------|
| `-kagura-lto-safe` | — | `false` | Enable passes during LTO / ThinLTO pipeline phases |
| `-kagura-o0-protect` | — | `false` | Enable the lightweight `-O0` subset (see below) |
| `-kagura-dwarf=<mode>` | `passes.dwarf` | `keep` | DWARF handling: `keep` / `strip` / `obfuscate` |
| `-kagura-build-id=<id>` | — | — | Build identifier mixed into PRNG seed for per-build key rotation |
| `-kagura-vtp` | — | `false` | RTTI / vtable protection (C++ ABI) |
| `-kagura-autoselect` | — | `false` | Score each function and pick its passes automatically |
| `-kagura-string-aggressive` | `string.aggressive` | `false` | Broader string coverage via startup decryption; see [Configuration](configuration.md#broader-string-protection) |

`-kagura-o0-protect` enables the passes whose cost stays bounded at `-O0`:
`str`, `str-aes`, `wstr`, `anti-debug`, and DWARF control when `-kagura-dwarf`
is not `keep`. Each still has to be requested; the flag only unblocks them.
Structural passes such as `fla`, `bcf` and `vm` never run at `-O0`.

## Build system

| Option | JSON key | Default | Description |
|:-------|:---------|:--------|:------------|
| `-kagura-config=<path>` | — | — | Path to JSON policy file |
| `-kagura-config-strict` | — | `false` | Fail the build on an unknown key in that file instead of warning |
| `-kagura-metrics` | — | `false` | Print before/after obfuscation metrics |
| `-kagura-symmap` | — | `false` | Emit symbol map after obfuscation |
| `-kagura-symmap-out=<path>` | — | `kagura_symbols.json` | Output file for symbol map |
| `-kagura-audit` | — | `false` | Emit observed pass changes and symbol audit |
| `-kagura-audit-out=<path>` | `audit_out` (root) | `kagura_audit.json` | Output file for audit log |

## Symbol filters

| Option | JSON key | Default | Description |
|:-------|:---------|:--------|:------------|
| `-kagura-protect=<pattern>` | `protect` (root) | — | Force-protect matching symbols (comma-separated, trailing-`*` glob) |
| `-kagura-deny=<pattern>` | `denylist` (root) | — | Exclude matching symbols from all obfuscation |
| `-kagura-allow=<pattern>` | `allowlist` (root) | — | Allowlist mode: only obfuscate matching symbols |

The three JSON list keys **merge** with their flags rather than replacing
them; see [Configuration](configuration.md#json-dsl).

## What is settable from a policy file

The `"tuning"` object reads exactly the five Core tuning rows above. The
`"passes"` object reads the per-pass enables (one key per `-kagura-<pass>`
flag) plus `"dwarf"`. `protect` / `denylist` / `allowlist` / `audit_out` are
root keys, not `"tuning"` keys.

The `"string"` object accepts `"aggressive"` for `-kagura-string-aggressive`.

Everything else on this page — `-kagura-lto-safe`, `-kagura-o0-protect`,
`-kagura-build-id`, `-kagura-vtp`, `-kagura-autoselect`, `-kagura-metrics`,
`-kagura-symmap`, `-kagura-symmap-out`, `-kagura-audit`, `-kagura-config` — has
no policy-file representation and must be passed on the command line. These
are build-invocation decisions rather than protection policy: which artefacts
to write, where, and whether the pipeline runs at all. Putting one in
`"passes"` or `"tuning"` now warns and names the flag to use instead, rather
than being silently ignored as it used to be.

## Reproducibility

The default seed is fixed; `-kagura-seed=<N>` selects another reproducible
stream, and `-kagura-build-id=<id>` mixes in a build identifier. Each pass and
function owns its derived stream, so processing another module or drawing in
another pass does not advance it. Relative source filenames retain their
project directories; absolute filenames use their basename for checkout
independence. Use stable project-relative input paths to distinguish
same-basename sources. Stream
partitioning changes the output of earlier Kagura versions at the same seed.
Pair with `scripts/ci/verify-reproducible.sh` to confirm two
builds produce byte-identical IR — see [Testing & Evaluation](testing.md).

## Audit results

`-kagura-audit` writes one JSON object per module to the append-only JSONL file.
`functions` lists functions whose IR or referenced global storage changed;
`pass_results` records observed registered transformation invocations with
`pass`, `scope`, `name` and `status` (`changed` or `unchanged`). Named passes,
the automatic pipeline, VTableProtection and the O0 protection subset use the
same observation mechanism. Audit collection adds IR fingerprinting work only
when enabled.

`unchanged` groups excluded functions, unsupported inputs and inputs with no
eligible transformation. A missing result means the pass was not observed;
for example, an LTO-gated or disabled pass has no entry. These observations do
not measure protection strength or establish working runtime enforcement.
The infrastructure passes for policy, metrics, debug information and symbol
maps are outside this transformation audit.

Writers serialize each complete record across threads and processes; an
unwritable or unlockable output fails compilation. Use a fresh output path per
build because appending does not remove records from previous builds.
