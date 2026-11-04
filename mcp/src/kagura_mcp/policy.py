"""Policy authoring and linting without LLVM or the MCP SDK.

Passes, tuning types and profiles come from the generated C++ registry catalog.
The remaining policy structure mirrors ConfigLoader.cpp; this is a stricter
authoring check, not a promise that a target supports every requested pass.
"""

from __future__ import annotations

from copy import deepcopy
from difflib import get_close_matches
from importlib.resources import files
import json


def catalog() -> dict:
    return json.loads(files("kagura_mcp").joinpath("catalog.json").read_text(encoding="utf-8"))


CLI_ONLY = {
    key: "-kagura-" + key.replace("_", "-") for key in (
        "vtp", "metrics", "autoselect", "lto_safe", "o0_protect", "symmap",
        "symmap_out", "build_id", "audit",
    )
}
LAYER_KEYS = {"profile", "passes", "tuning", "string", "allowlist", "denylist", "protect"}


def validate_config(config: object) -> dict:
    """Return all authoring errors, including settings LLVM would silently ignore."""
    data = catalog()
    errors: list[dict] = []

    def error(path, message):
        errors.append({"path": path, "message": message})

    def unknown(key, path, valid):
        canonical = key.replace("-", "_")
        if canonical in CLI_ONLY:
            message = f"Command-line-only option; use {CLI_ONLY[canonical]} instead."
        else:
            match = get_close_matches(canonical, sorted(valid), n=1, cutoff=0.6)
            message = "Unknown key; the compiler ignores this setting."
            if match:
                message += f" Did you mean '{match[0]}'?"
        error(path, message)

    def layer(obj, path, root=False):
        if not isinstance(obj, dict):
            error(path, "Expected a JSON object.")
            return
        valid = LAYER_KEYS | ({"flavors", "audit_out", "_generated"} if root else set())
        for key in obj:
            if key not in valid:
                unknown(key, f"{path}.{key}", valid)
        if "profile" in obj:
            profile = obj["profile"]
            if not isinstance(profile, str) or profile.upper() not in {*data["profiles"], "CUSTOM"}:
                error(f"{path}.profile", "Expected FAST, BALANCED, STRONG or CUSTOM.")
        for section, known in (("passes", data["passes"]), ("tuning", data["tuning"]),
                               ("string", data["string"])):
            if section not in obj:
                continue
            values = obj[section]
            location = f"{path}.{section}"
            if not isinstance(values, dict):
                error(location, "Expected a JSON object.")
                continue
            seen = set()
            for spelling, value in values.items():
                key = spelling.replace("-", "_")
                item_path = f"{location}.{spelling}"
                if key in seen:
                    error(item_path, "Duplicate key after hyphen/underscore normalization.")
                seen.add(key)
                if section == "passes" and key == "dwarf":
                    if value not in ("keep", "strip", "obfuscate"):
                        error(item_path, "Expected keep, strip or obfuscate.")
                elif key not in known:
                    unknown(spelling, item_path, set(known) | ({"dwarf"} if section == "passes" else set()))
                elif section == "tuning":
                    maximum = min((1 << (64 if known[key]["type"] == "uint64_t" else 32)) - 1,
                                  (1 << 63) - 1)  # LLVM JSON getAsInteger is signed.
                    if type(value) is not int or not 0 <= value <= maximum:
                        error(item_path, f"Expected an integer in [0, {maximum}].")
                    elif "[0-100]" in known[key]["description"] and value > 100:
                        error(item_path, "Expected a probability in [0, 100].")
                elif type(value) is not bool:
                    error(item_path, "Expected a boolean.")
        for key in ("allowlist", "denylist", "protect"):
            if key in obj and (not isinstance(obj[key], list) or
                               any(not isinstance(x, str) or not x or "," in x for x in obj[key])):
                error(f"{path}.{key}", "Expected a list of nonempty symbol patterns without commas.")
        if root:
            if "audit_out" in obj and (not isinstance(obj["audit_out"], str) or not obj["audit_out"]):
                error(f"{path}.audit_out", "Expected a nonempty output path; also pass -kagura-audit.")
            if "flavors" in obj:
                if not isinstance(obj["flavors"], dict):
                    error(f"{path}.flavors", "Expected an object of named policy layers.")
                else:
                    for name, flavor in obj["flavors"].items():
                        layer(flavor, f"{path}.flavors.{name}")

    layer(config, "$", root=True)
    return {"valid": not errors, "errors": errors}


def load_config(text: str) -> object:
    """Reject duplicate literal keys too, before json.loads discards them."""
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"Duplicate JSON key: {key}")
            result[key] = value
        return result

    def invalid_constant(value):
        raise ValueError(f"Invalid JSON constant: {value}")

    return json.loads(text, object_pairs_hook=unique, parse_constant=invalid_constant)


def generate_config(profile: str = "BALANCED", passes: dict | None = None,
                    tuning: dict | None = None, protect: list[str] | None = None,
                    denylist: list[str] | None = None, allowlist: list[str] | None = None,
                    aggressive_strings: bool = False, scoped_strings: bool = False) -> dict:
    data = catalog()
    profile = profile.upper()
    if profile not in {*data["profiles"], "CUSTOM"}:
        raise ValueError("Expected FAST, BALANCED, STRONG or CUSTOM.")
    config = {"profile": profile}
    if profile != "CUSTOM":
        config.update(deepcopy(data["profiles"][profile]))
    for section, overrides in (("passes", passes), ("tuning", tuning)):
        if overrides:
            # Validate aliases before merging so contradictory spellings cannot disappear.
            checked = validate_config({section: overrides})
            if not checked["valid"]:
                raise ValueError(json.dumps(checked["errors"]))
            config.setdefault(section, {}).update({k.replace("-", "_"): v for k, v in overrides.items()})
    for key, values in (("protect", protect), ("denylist", denylist), ("allowlist", allowlist)):
        if values is not None:
            config[key] = values
    if aggressive_strings:
        config.setdefault("string", {})["aggressive"] = True
    if scoped_strings:
        config.setdefault("string", {})["scoped"] = True
    checked = validate_config(config)
    if not checked["valid"]:
        raise ValueError(json.dumps(checked["errors"]))
    flags = []
    enabled = config.get("passes", {})
    if any(enabled.get(p) for p in ("anti_debug", "str_aes", "vm", "tamper", "ci", "telemetry")):
        flags.append("Link kagura_runtime for the enabled runtime-dependent passes.")
    if scoped_strings:
        flags.append("Scoped buffers require readonly, noncapturing, nonthrowing consumers; escaped or unsupported literals remain persistent.")
    flags.append("Verify target support and measure overhead; this policy does not prove protection strength.")
    return {"config": config, "notes": flags}
