# kagura-mcp

Local stdio MCP server for Kagura policy authoring and build inspection.
Python 3.10+ is required. LLVM is only needed for environment probes and binary
inspection, not for listing passes or generating/validating policies.

## Run

From a Kagura checkout, with [uv](https://docs.astral.sh/uv/):

```bash
uv run --project /absolute/path/to/kagura/mcp kagura-mcp \
  --workspace /absolute/path/to/your-app
```

Or install into a virtual environment:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install ./mcp
.venv/bin/kagura-mcp --workspace /absolute/path/to/your-app
```

The process speaks MCP on stdin/stdout; it waits for a client and is not an
interactive terminal command. `kagura-mcp --help` shows startup options.
No client settings are installed automatically.

## Connect a client

Clients accepting an `mcpServers` JSON object can use the following configuration.
Replace all paths, and use an absolute path to `uv` if the client's PATH omits it.

```json
{
  "mcpServers": {
    "kagura": {
      "command": "uv",
      "args": [
        "run", "--project", "/absolute/path/to/kagura/mcp",
        "kagura-mcp", "--workspace", "/absolute/path/to/your-app"
      ]
    }
  }
}
```

For clients with another configuration format, use the same executable and
argument array. The installed `.venv/bin/kagura-mcp` executable can replace uv.

## Tools

| Tool | Inputs and result |
|:-----|:------------------|
| `doctor` | Tool paths and versions, kagura-opt load probe, compiler-driver readiness, prerequisites and warnings |
| `list_passes` | Registry-derived pass descriptions, canonical keys, CLI flags, numeric defaults and exact profiles |
| `generate_config` | Profile, pass/tuning overrides, symbol lists and optional aggressive/scoped strings → policy proposal and notes |
| `validate_config` | Exactly one `config` object or workspace `path` → errors with JSON paths |
| `inspect_audit` | JSONL `path`, optional module/function substring filters and offset/limit → counts, function page and malformed-line errors |
| `compare_binaries` | Baseline/obfuscated paths → size/section/symbol/string count deltas and bounded symbol samples |

Example workflow: ask your client to generate a BALANCED policy restricted to
your payment functions, review the result, save it using the client's file
tools, then build with your existing integration. Ask Kagura to inspect the
audit log and compare the baseline/release binaries afterward. This server
does not compile, apply passes, write policies or modify binaries.

`protect` force-selects patterns; `allowlist` restricts normal selection.
All built-in profiles enable anti-debug and require `kagura_runtime`.
`-kagura-audit`, `-kagura-symmap` and `-kagura-build-id` are CLI-only options,
not root policy keys. An `audit_out` path still needs `-kagura-audit`.

`generate_config(scoped_strings=true)` emits `"string": {"scoped": true}`.
It can be combined with `aggressive_strings`; enable `str` or `str_aes` as well.
Only eligible readonly, noncapturing, nonthrowing consumers use wiped stack
buffers; unsupported uses keep persistent plaintext. The Python CLI exposes
`config-gen --scoped-strings --aggressive-strings` for the same policy options.
`doctor` reports `compiler_driver_ready` only when the driver is present and
clang/kagura-opt start with matching supported upstream LLVM majors.
Use `kagura --config-check` for the compiler's native strict check and `kagura`
for source compilation. The MCP tools continue to perform read-only inspection.

The authoring validator is deliberately stricter than the compiler loader:
unknown root keys, wrong types, probabilities outside 0–100, duplicate alias
spellings and invalid flavor blocks fail validation. It does not resolve
command-line precedence, select an active flavor, or assert target support.

## File access and external tools

`--workspace` defaults to the current directory. Every file input must resolve
inside it, including through symlinks. JSON/log inputs are limited to 16 MiB;
each binary to 256 MiB. Audit pages contain at most 500 function entries,
with at most 50 malformed-line details; symbol samples contain at most 200 names.
Audit absence does not prove a function was unprotected, and appended logs
can contain repeated entries from earlier builds.

Binary comparison uses `llvm-nm`/`nm`, `llvm-size`/`size` and `strings` on PATH,
with argument arrays and a 30-second timeout per invocation. Put a matching
LLVM bin directory on the client's PATH to select its inspection tools.
`--llvm-bin /path/to/llvm/bin` selects tools for **doctor's diagnosis**;
`--plugin /path/to/KaguraObfuscator.dylib` selects the plugin for its kagura-opt
probe. kagura-opt must be on PATH or at `<workspace>/build/bin/kagura-opt`.

All tools are read-only. Logs, symbols and file paths returned to the client
may include application details. The server opens no network listener.
Counts and size differences are observations, not a protection-strength score.

## Development

```bash
python3 scripts/ci/gen-profiles.py           # regenerate profiles + Python catalog
python3 scripts/ci/gen-profiles.py --check
uv run --project mcp python -m unittest discover -s mcp/tests -v
```

`PassRegistry.def` and `Profiles.def` remain the source of truth. The generated
catalog is bundled in the wheel, so installed servers do not need the C++ tree.
The existing `kagura-cli.py` and `kagura-diff.py` use the same Python modules
without importing the MCP SDK. Keep `mcp/src` alongside `scripts/cli` when
using those scripts directly from a checkout, or install the wheel first.
