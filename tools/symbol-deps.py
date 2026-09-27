#!/usr/bin/env python3
"""Inspect object/archive symbols and derive static dependency graphs.

The report is based on link-visible symbols, not source includes or runtime
callback registration. Build without LTO for the most useful results.
"""

from __future__ import annotations

import argparse
import collections
import dataclasses
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
from typing import Iterable, TextIO


@dataclasses.dataclass(frozen=True)
class Symbol:
    artifact: str
    member: str
    name: str
    type: str
    binding: str
    state: str

    @property
    def unit(self) -> str:
        if self.member == self.artifact:
            return self.artifact
        return f"{self.artifact}({self.member})"

    @property
    def kind(self) -> str:
        if self.state == "undefined":
            return "undefined"
        if self.type.upper() == "T":
            return "function"
        return "object"


@dataclasses.dataclass(frozen=True)
class InputArtifact:
    label: str
    path: Path


def parse_la(path: Path) -> Path:
    """Resolve a Libtool .la file to the archive named by old_library."""
    old_library = None
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            match = re.fullmatch(r"old_library='([^']*)'", line.strip())
            if match:
                old_library = match.group(1)
                break
    except OSError as error:
        raise ValueError(f"cannot read {path}: {error}") from error

    if not old_library:
        raise ValueError(f"{path} does not name an old_library")

    candidates = (path.parent / ".libs" / old_library, path.parent / old_library)
    for candidate in candidates:
        if candidate.exists():
            return candidate
    raise ValueError(
        f"cannot find {old_library} for {path}; looked in "
        + ", ".join(str(candidate) for candidate in candidates)
    )


def resolve_inputs(paths: Iterable[str]) -> list[InputArtifact]:
    artifacts: list[InputArtifact] = []
    labels: set[str] = set()
    for value in paths:
        original = Path(value)
        path = parse_la(original) if original.suffix == ".la" else original
        if not path.exists():
            raise ValueError(f"input does not exist: {path}")
        label = str(original)
        if label in labels:
            raise ValueError(f"duplicate input label: {label}")
        labels.add(label)
        artifacts.append(InputArtifact(label=label, path=path))
    return artifacts


def classify(symbol_type: str) -> tuple[str, str]:
    """Return (binding, state) for a portable nm symbol type letter."""
    if symbol_type in {"U"}:
        return "global", "undefined"
    if symbol_type in {"w", "v"}:
        return "weak", "undefined"
    if symbol_type in {"W", "V"}:
        return "weak", "defined"
    if symbol_type == "C":
        return "common", "defined"
    if symbol_type == "u":  # GNU unique global
        return "global", "defined"
    if symbol_type.isupper():
        return "global", "defined"
    return "local", "defined"


def parse_location(location: str, artifact: InputArtifact) -> str:
    """Extract a member name from GNU or Darwin nm -A -P locations."""
    location = location.rstrip(":")
    bracket = re.search(r"\[([^]]+)\]$", location)
    if bracket:
        return bracket.group(1)

    # GNU nm commonly emits archive:member:.
    real = str(artifact.path)
    if location.startswith(real + ":"):
        member = location[len(real) + 1 :].rstrip(":")
        if member:
            return member

    return artifact.label


def read_symbols(nm: str, artifact: InputArtifact) -> list[Symbol]:
    command = shlex.split(nm) + ["-A", "-P", str(artifact.path)]
    try:
        result = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except OSError as error:
        raise ValueError(f"cannot execute {command[0]}: {error}") from error
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ValueError(f"{' '.join(command)} failed: {detail}")

    symbols: list[Symbol] = []
    for line_number, line in enumerate(result.stdout.splitlines(), 1):
        fields = line.split()
        if len(fields) < 3:
            continue
        location, name, symbol_type = fields[:3]
        if len(symbol_type) != 1:
            continue
        binding, state = classify(symbol_type)
        symbols.append(
            Symbol(
                artifact=artifact.label,
                member=parse_location(location, artifact),
                name=name,
                type=symbol_type,
                binding=binding,
                state=state,
            )
        )
    return symbols


def provider_map(symbols: Iterable[Symbol]) -> dict[str, list[Symbol]]:
    providers: dict[str, list[Symbol]] = collections.defaultdict(list)
    for symbol in symbols:
        if symbol.state == "defined" and symbol.binding != "local":
            providers[symbol.name].append(symbol)
    return providers


def dependencies(
    symbols: Iterable[Symbol], scope: str
) -> tuple[dict[tuple[str, str], set[str]], dict[str, set[str]], dict[str, set[str]]]:
    all_symbols = list(symbols)
    providers = provider_map(all_symbols)
    edges: dict[tuple[str, str], set[str]] = collections.defaultdict(set)
    unresolved: dict[str, set[str]] = collections.defaultdict(set)
    ambiguous: dict[str, set[str]] = collections.defaultdict(set)

    def identity(symbol: Symbol) -> str:
        return symbol.artifact if scope == "archive" else symbol.unit

    for symbol in all_symbols:
        if symbol.state != "undefined" or symbol.binding == "local":
            continue
        consumer = identity(symbol)
        candidates = providers.get(symbol.name, [])
        if not candidates:
            unresolved[consumer].add(symbol.name)
            continue
        provider_names = {identity(candidate) for candidate in candidates}
        if consumer in provider_names:
            # Prefer a definition in the same archive/object scope. Reporting
            # duplicate definitions elsewhere as dependencies would create
            # false edges, especially for weak and stub implementations.
            continue
        for provider in provider_names:
            edges[(consumer, provider)].add(symbol.name)
        if len(provider_names) > 1:
            ambiguous[consumer].add(symbol.name)

    return edges, unresolved, ambiguous


def symbol_dict(symbol: Symbol) -> dict[str, str]:
    return {
        "artifact": symbol.artifact,
        "member": symbol.member,
        "unit": symbol.unit,
        "name": symbol.name,
        "type": symbol.type,
        "binding": symbol.binding,
        "state": symbol.state,
        "kind": symbol.kind,
    }


def report_json(
    out: TextIO,
    artifacts: list[InputArtifact],
    symbols: list[Symbol],
    scope: str,
) -> None:
    edges, unresolved, ambiguous = dependencies(symbols, scope)
    document = {
        "scope": scope,
        "inputs": [
            {"label": artifact.label, "resolved_path": str(artifact.path)}
            for artifact in artifacts
        ],
        "dependencies": [
            {"consumer": consumer, "provider": provider, "symbols": sorted(names)}
            for (consumer, provider), names in sorted(edges.items())
        ],
        "unresolved": {name: sorted(names) for name, names in sorted(unresolved.items())},
        "ambiguous": {name: sorted(names) for name, names in sorted(ambiguous.items())},
        "symbols": [symbol_dict(symbol) for symbol in symbols],
    }
    json.dump(document, out, indent=2, sort_keys=False)
    out.write("\n")


def mermaid_id(index: int) -> str:
    return f"unit{index}"


def report_dot(out: TextIO, symbols: list[Symbol], scope: str) -> None:
    edges, _, _ = dependencies(symbols, scope)
    names = sorted({name for edge in edges for name in edge})
    identifiers = {name: f"n{index}" for index, name in enumerate(names)}
    out.write("digraph symbol_dependencies {\n")
    out.write("  rankdir=LR;\n")
    for name in names:
        escaped = name.replace("\\", "\\\\").replace('"', '\\"')
        out.write(f'  {identifiers[name]} [label="{escaped}"];\n')
    for (consumer, provider), edge_symbols in sorted(edges.items()):
        out.write(
            f"  {identifiers[consumer]} -> {identifiers[provider]} "
            f'[label="{len(edge_symbols)}"];\n'
        )
    out.write("}\n")


def report_mermaid(out: TextIO, symbols: list[Symbol], scope: str) -> None:
    edges, _, _ = dependencies(symbols, scope)
    names = sorted({name for edge in edges for name in edge})
    identifiers = {name: mermaid_id(index) for index, name in enumerate(names)}
    out.write("flowchart LR\n")
    for name in names:
        # MDS Mermaid labels use Markdown strings and prohibit arbitrary HTML.
        escaped = name.replace('"', "'").replace("`", "'")
        out.write(f'    {identifiers[name]}["`{escaped}`"]\n')
    out.write("\n")
    for (consumer, provider), edge_symbols in sorted(edges.items()):
        out.write(
            f"    {identifiers[consumer]} -->|{len(edge_symbols)} symbols| "
            f"{identifiers[provider]}\n"
        )
    if names:
        out.write("\n    classDef prose text-align:left\n")
        out.write("    class " + ",".join(identifiers.values()) + " prose\n")


def format_symbols(names: Iterable[str], limit: int) -> str:
    values = sorted(names)
    if not values:
        return "—"
    if limit and len(values) > limit:
        shown = values[:limit]
        return ", ".join(f"`{name}`" for name in shown) + f", … ({len(values) - limit} more)"
    return ", ".join(f"`{name}`" for name in values)


def report_markdown(
    out: TextIO,
    artifacts: list[InputArtifact],
    symbols: list[Symbol],
    scope: str,
    include_symbols: bool,
    symbol_limit: int,
) -> None:
    edges, unresolved, ambiguous = dependencies(symbols, scope)
    out.write("# Static Symbol Dependency Report\n\n")
    out.write(
        f"Dependency scope: **{scope}**. An arrow is directed from the unit "
        "containing an undefined symbol to a unit that defines it.\n\n"
    )
    out.write("## Inputs\n\n")
    for artifact in artifacts:
        if artifact.label == str(artifact.path):
            out.write(f"- `{artifact.label}`\n")
        else:
            out.write(f"- `{artifact.label}` → `{artifact.path}`\n")

    out.write("\n## Dependency graph\n\n```mermaid\n")
    report_mermaid(out, symbols, scope)
    out.write("```\n\n")

    out.write("## Dependency edges\n\n")
    if not edges:
        out.write("No cross-unit dependencies were found.\n\n")
    else:
        out.write("| Consumer | Provider | Symbols |\n")
        out.write("|---|---|---|\n")
        for (consumer, provider), names in sorted(edges.items()):
            out.write(
                f"| `{consumer}` | `{provider}` | "
                f"{format_symbols(names, symbol_limit)} |\n"
            )
        out.write("\n")

    out.write("## Unresolved external symbols\n\n")
    if not unresolved:
        out.write("Every undefined symbol has a provider in the supplied inputs.\n\n")
    else:
        for consumer, names in sorted(unresolved.items()):
            out.write(f"### `{consumer}`\n\n{format_symbols(names, symbol_limit)}\n\n")

    if ambiguous:
        out.write("## Symbols with multiple candidate providers\n\n")
        for consumer, names in sorted(ambiguous.items()):
            out.write(f"- `{consumer}`: {format_symbols(names, symbol_limit)}\n")
        out.write("\n")

    if include_symbols:
        out.write("## Artifact exports\n\n")
        by_artifact: dict[str, list[Symbol]] = collections.defaultdict(list)
        by_unit: dict[str, list[Symbol]] = collections.defaultdict(list)
        for symbol in symbols:
            by_artifact[symbol.artifact].append(symbol)
            by_unit[symbol.unit].append(symbol)

        for artifact, artifact_symbols in sorted(by_artifact.items()):
            exported_functions = {
                symbol.name
                for symbol in artifact_symbols
                if symbol.state == "defined"
                and symbol.binding != "local"
                and symbol.kind == "function"
            }
            exported_objects = {
                symbol.name
                for symbol in artifact_symbols
                if symbol.state == "defined"
                and symbol.binding != "local"
                and symbol.kind != "function"
            }
            out.write(f"### `{artifact}`\n\n")
            out.write(
                f"- Link-visible functions ({len(exported_functions)}): "
                f"{format_symbols(exported_functions, symbol_limit)}\n"
            )
            out.write(
                f"- Other link-visible definitions ({len(exported_objects)}): "
                f"{format_symbols(exported_objects, symbol_limit)}\n\n"
            )

        out.write("## Object-file symbol inventory\n\n")
        for unit, unit_symbols in sorted(by_unit.items()):
            global_functions = [
                symbol
                for symbol in unit_symbols
                if symbol.state == "defined"
                and symbol.binding != "local"
                and symbol.kind == "function"
            ]
            global_objects = [
                symbol
                for symbol in unit_symbols
                if symbol.state == "defined"
                and symbol.binding != "local"
                and symbol.kind != "function"
            ]
            local_functions = [
                symbol
                for symbol in unit_symbols
                if symbol.state == "defined"
                and symbol.binding == "local"
                and symbol.kind == "function"
            ]
            local_objects = [
                symbol
                for symbol in unit_symbols
                if symbol.state == "defined"
                and symbol.binding == "local"
                and symbol.kind != "function"
            ]
            undefined_symbols = [
                symbol for symbol in unit_symbols if symbol.state == "undefined"
            ]
            out.write(f"### `{unit}`\n\n")
            out.write(
                f"- Link-visible functions ({len(global_functions)}): "
                f"{format_symbols((s.name for s in global_functions), symbol_limit)}\n"
            )
            out.write(
                f"- Other link-visible definitions ({len(global_objects)}): "
                f"{format_symbols((s.name for s in global_objects), symbol_limit)}\n"
            )
            out.write(
                f"- Local functions ({len(local_functions)}): "
                f"{format_symbols((s.name for s in local_functions), symbol_limit)}\n"
            )
            out.write(f"- Other local definitions: {len(local_objects)}\n")
            out.write(
                f"- Undefined references ({len(undefined_symbols)}): "
                f"{format_symbols((s.name for s in undefined_symbols), symbol_limit)}\n\n"
            )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="derive object or archive dependencies from nm symbol tables"
    )
    parser.add_argument("inputs", nargs="+", help=".o, .a, or Libtool .la files")
    parser.add_argument(
        "--nm",
        default=os.environ.get("NM", "nm"),
        help="nm command, including optional arguments (default: $NM or nm)",
    )
    parser.add_argument(
        "--scope",
        choices=("archive", "object"),
        default="archive",
        help="aggregate dependencies by input archive or archive member",
    )
    parser.add_argument(
        "--format",
        choices=("markdown", "json", "dot", "mermaid"),
        default="markdown",
        help="report format",
    )
    parser.add_argument("-o", "--output", help="write the report to this file")
    parser.add_argument(
        "--include-symbols",
        action="store_true",
        help="include per-object symbol inventories in Markdown output",
    )
    parser.add_argument(
        "--symbol-limit",
        type=int,
        default=30,
        help="maximum symbols shown in each Markdown list; 0 means unlimited",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.symbol_limit < 0:
        parser.error("--symbol-limit must be nonnegative")

    try:
        artifacts = resolve_inputs(args.inputs)
        symbols = [
            symbol
            for artifact in artifacts
            for symbol in read_symbols(args.nm, artifact)
        ]
    except ValueError as error:
        parser.error(str(error))

    if args.output:
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        out_context = output_path.open("w", encoding="utf-8")
    else:
        out_context = None

    out = out_context or sys.stdout
    try:
        if args.format == "json":
            report_json(out, artifacts, symbols, args.scope)
        elif args.format == "dot":
            report_dot(out, symbols, args.scope)
        elif args.format == "mermaid":
            report_mermaid(out, symbols, args.scope)
        else:
            report_markdown(
                out,
                artifacts,
                symbols,
                args.scope,
                args.include_symbols,
                args.symbol_limit,
            )
    finally:
        if out_context:
            out_context.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
