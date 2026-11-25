"""Readers for the compiler's per-translation-unit reports."""

from collections.abc import Callable, Iterator
import json
from typing import BinaryIO, TextIO

from .policy import load_config


def _audit_record(line: bytes) -> dict:
    record = load_config(line.decode("utf-8"))
    if not isinstance(record, dict) or not isinstance(record.get("module"), str):
        raise ValueError("Expected a record with a module string.")
    functions = record.get("functions")
    if not isinstance(functions, list):
        raise ValueError("Expected a functions array.")
    for item in functions:
        if not isinstance(item, dict) or not isinstance(item.get("name"), str):
            raise ValueError("Expected a function with a name string.")
        passes = item.get("passes")
        if not isinstance(passes, list) or not all(isinstance(p, str) for p in passes):
            raise ValueError("Expected function passes to be an array of strings.")
    return record


def audit_records(stream: BinaryIO, on_error: Callable[[int, str], None]) -> Iterator[dict]:
    for number, line in enumerate(stream, 1):
        if not line.strip():
            continue
        try:
            record = _audit_record(line)
        except (ValueError, RecursionError) as exc:
            on_error(number, str(exc))
            continue
        yield record


def _symbol_records(stream: TextIO) -> Iterator[object]:
    for first in stream:
        if first.strip():
            break
    else:
        return

    try:
        record = json.loads(first)
    except json.JSONDecodeError:
        # Older standalone maps can be a single pretty-printed JSON document.
        stream.seek(0)
        yield json.load(stream)
        return

    yield record
    for line in stream:
        if line.strip():
            yield json.loads(line)


def symbol_pairs(stream: TextIO) -> Iterator[tuple[str, str]]:
    for record in _symbol_records(stream):
        symbols = record.get("symbols", record) if isinstance(record, dict) else record
        if isinstance(symbols, dict):
            for original, obfuscated in symbols.items():
                if not isinstance(obfuscated, str):
                    raise ValueError("Expected the symbol mapping to contain string values.")
                yield original, obfuscated
        elif isinstance(symbols, list):
            for item in symbols:
                if not isinstance(item, dict):
                    raise ValueError("Expected a symbol object.")
                original, obfuscated = item.get("original", "?"), item.get("obfuscated", "?")
                if not isinstance(original, str) or not isinstance(obfuscated, str):
                    raise ValueError("Expected string symbol names.")
                yield original, obfuscated
        else:
            raise ValueError("Expected a symbol map object or array.")
