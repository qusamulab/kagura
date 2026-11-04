"""Local stdio MCP interface. stdout is reserved for the MCP transport."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP
from mcp.types import ToolAnnotations

from .inspection import Workspace, doctor as inspect_environment
from .policy import catalog, generate_config as make_config, validate_config as check_config


def create_server(workspace: Path, llvm_bin: Path | None = None,
                  plugin: Path | None = None) -> FastMCP:
    scope = Workspace(workspace)
    server = FastMCP("kagura-mcp", instructions=(
        "Kagura configuration and read-only build inspection. All file paths are scoped to "
        f"{scope.root}. Generated policies are proposals, not saved files. "
        "Check target support, runtime linkage and actual performance before shipping."
    ))
    readonly = ToolAnnotations(readOnlyHint=True, destructiveHint=False,
                               idempotentHint=True, openWorldHint=False)

    @server.tool(annotations=readonly, structured_output=True)
    def doctor() -> dict[str, Any]:
        """Check local LLVM tools, kagura-opt/plugin loading and inspection prerequisites."""
        return inspect_environment(scope, llvm_bin, plugin)

    @server.tool(annotations=readonly, structured_output=True)
    def list_passes() -> dict[str, Any]:
        """List canonical pass keys, CLI flags, descriptions, tuning and exact strength profiles.

        Descriptions may mention target requirements; this catalog does not assert
        compatibility with a particular platform or measure protection strength.
        """
        return catalog()

    @server.tool(annotations=readonly, structured_output=True)
    def generate_config(profile: str = "BALANCED", passes: dict[str, bool | str] | None = None,
                        tuning: dict[str, int] | None = None, protect: list[str] | None = None,
                        denylist: list[str] | None = None, allowlist: list[str] | None = None,
                        aggressive_strings: bool = False, scoped_strings: bool = False) -> dict[str, Any]:
        """Return a validated JSON policy proposal without writing files.

        Choose FAST, BALANCED, STRONG or CUSTOM. Pass and tuning overrides accept
        hyphens or underscores. dwarf is the string-valued passes exception.
        protect force-selects patterns; allowlist restricts the normal selection.
        """
        return make_config(profile, passes, tuning, protect, denylist, allowlist,
                           aggressive_strings, scoped_strings)

    @server.tool(annotations=readonly, structured_output=True)
    def validate_config(config: dict[str, Any] | None = None, path: str | None = None) -> dict[str, Any]:
        """Lint exactly one inline policy or workspace JSON file without compiling.

        Checks names, types, numeric ranges, flavors and ignored CLI-only settings.
        This authoring check is stricter than ConfigLoader and cannot prove target support.
        """
        if (config is None) == (path is None):
            raise ValueError("Provide exactly one of config or path.")
        return scope.validate(path) if path is not None else check_config(config)

    @server.tool(annotations=readonly, structured_output=True)
    def inspect_audit(path: str = "kagura_audit.json", module: str = "", function: str = "",
                      offset: int = 0, limit: int = 100) -> dict[str, Any]:
        """Read newline-delimited audit records, summarize pass counts and page function entries.

        module and function are case-sensitive substring filters. Malformed lines
        are reported with line numbers and complete=false; they are never silently omitted.
        """
        return scope.audit(path, module, function, offset, limit)

    @server.tool(annotations=readonly, structured_output=True)
    def compare_binaries(baseline: str, obfuscated: str, min_string_length: int = 4,
                         sample_limit: int = 50) -> dict[str, Any]:
        """Compare file/section sizes, nm symbol counts and printable string counts.

        Requires nm, size and strings (LLVM variants preferred on PATH). Files stay
        inside the workspace; samples are bounded. Does not prove protection strength.
        """
        return scope.compare(baseline, obfuscated, min_string_length, sample_limit)

    return server


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, default=Path.cwd(), help="Root for all tool file reads")
    parser.add_argument("--llvm-bin", type=Path, help="LLVM bin directory used for environment diagnosis")
    parser.add_argument("--plugin", type=Path, help="Plugin path used for the kagura-opt load probe")
    args = parser.parse_args()
    create_server(args.workspace, args.llvm_bin, args.plugin).run(transport="stdio")


if __name__ == "__main__":
    main()
