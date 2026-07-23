#!/usr/bin/env python3
"""Create the source-pinned GQL clause feature corpus from openCypher TCK.

The upstream Gherkin scenarios remain useful as a breadth inventory, but their
query language is not ISO GQL. This importer applies only transformations with
an unambiguous spelling-level mapping and records every remaining semantic gap
in a scenario manifest. It intentionally does not mark scenarios executable.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path


UPSTREAM_URL = "https://github.com/opencypher/openCypher"
PINNED_REVISION = "677cbafabb8c3c5eed458fd3b1ec0daec8d67d23"
MODIFICATION_NOTICE = (
    "# Modified by DuckGQL: query text uses the mechanical GQL mappings "
    "documented in test/features/README.md; scenario semantics remain unverified."
)


@dataclass
class Scenario:
    source_path: str
    target_path: str
    source_line: int
    name: str
    text: str


def git_revision(repository: Path) -> str:
    result = subprocess.run(
        ["git", "-C", str(repository), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def query_word_rewrite(line: str) -> str:
    """Rewrite tokens outside quoted strings and line comments."""
    result: list[str] = []
    previous_word = ""
    index = 0
    while index < len(line):
        character = line[index]
        if character in {"'", '"', "`"}:
            quote = character
            start = index
            index += 1
            while index < len(line):
                if line[index] == quote:
                    index += 1
                    if index < len(line) and line[index] == quote:
                        index += 1
                        continue
                    break
                index += 1
            result.append(line[start:index])
            continue
        if character == "/" and index + 1 < len(line) and line[index + 1] == "/":
            result.append(line[index:])
            break
        if character.isalpha() or character == "_":
            start = index
            index += 1
            while index < len(line) and (line[index].isalnum() or line[index] == "_"):
                index += 1
            word = line[start:index]
            upper = word.upper()
            replacement = word
            if upper == "CREATE" and previous_word != "ON":
                replacement = "INSERT"
            elif upper == "SKIP":
                replacement = "OFFSET"
            elif upper == "ID":
                next_index = index
                while next_index < len(line) and line[next_index].isspace():
                    next_index += 1
                if next_index < len(line) and line[next_index] == "(":
                    replacement = "element_id"
            result.append(replacement)
            previous_word = upper
            continue
        result.append(character)
        index += 1
    return "".join(result)


def translate_query_line(line: str) -> str:
    return query_word_rewrite(line)


def top_level_words(text: str) -> list[tuple[int, int, str]]:
    words: list[tuple[int, int, str]] = []
    stack: list[str] = []
    quote = ""
    index = 0
    pairs = {"(": ")", "[": "]", "{": "}"}
    while index < len(text):
        character = text[index]
        if quote:
            if character == quote:
                if index + 1 < len(text) and text[index + 1] == quote:
                    index += 2
                    continue
                quote = ""
            index += 1
            continue
        if character in {"'", '"', "`"}:
            quote = character
            index += 1
            continue
        if character in pairs:
            stack.append(pairs[character])
            index += 1
            continue
        if stack and character == stack[-1]:
            stack.pop()
            index += 1
            continue
        if not stack and (character.isalpha() or character == "_"):
            start = index
            index += 1
            while index < len(text) and (text[index].isalnum() or text[index] == "_"):
                index += 1
            words.append((start, index, text[start:index].upper()))
            continue
        index += 1
    return words


def split_top_level(text: str) -> list[str]:
    result: list[str] = []
    start = 0
    stack: list[str] = []
    quote = ""
    pairs = {"(": ")", "[": "]", "{": "}"}
    for index, character in enumerate(text):
        if quote:
            if character == quote:
                quote = ""
            continue
        if character in {"'", '"', "`"}:
            quote = character
        elif character in pairs:
            stack.append(pairs[character])
        elif stack and character == stack[-1]:
            stack.pop()
        elif character == "," and not stack:
            result.append(text[start:index].strip())
            start = index + 1
    result.append(text[start:].strip())
    return [item for item in result if item]


def translate_with_line(line: str, ordinal: int) -> list[str]:
    match = re.match(r"^(\s*)WITH\s+(.+)$", line, re.I)
    if not match:
        return [line]
    indentation, body = match.groups()
    words = top_level_words(body)
    suffix_start: int | None = None
    suffix_word = ""
    clause_words = {"RETURN", "MATCH", "OPTIONAL", "CALL", "MERGE", "INSERT", "DELETE", "DETACH", "SET", "REMOVE", "ORDER", "WHERE", "OFFSET", "LIMIT", "UNION"}
    for start, _, word in words:
        if start > 0 and word in clause_words:
            suffix_start = start
            suffix_word = word
            break
    projection = body if suffix_start is None else body[:suffix_start].rstrip()
    suffix = "" if suffix_start is None else body[suffix_start:].lstrip()
    if projection.upper().startswith("DISTINCT "):
        projection = projection[len("DISTINCT ") :]

    definitions = [f"__gql_with_scope_{ordinal} = 1"]
    for item_index, item in enumerate(split_top_level(projection), start=1):
        item_words = top_level_words(item)
        aliases = [(start, end) for start, end, word in item_words if word == "AS"]
        if aliases:
            alias_start, alias_end = aliases[-1]
            expression = item[:alias_start].strip()
            alias = item[alias_end:].strip()
            if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", alias) and expression != alias:
                definitions.append(f"{alias} = {expression}")
                continue
        safe_name = re.sub(r"[^A-Za-z0-9_]", "_", item).strip("_")[:24] or "value"
        definitions.append(f"__gql_with_{ordinal}_{item_index}_{safe_name} = {item}")

    translated = [f"{indentation}LET " + ", ".join(definitions)]
    if suffix:
        if suffix_word == "WHERE":
            suffix = "FILTER " + suffix
        translated.append(indentation + suffix)
    return translated


def translate_query_block(lines: list[str]) -> list[str]:
    text = "\n".join(lines)
    text = re.sub(
        r"(?ims)^(\s*)UNWIND\s+(.+?)\s+AS\s+([A-Za-z_][A-Za-z0-9_]*)\s*$",
        lambda match: f"{match.group(1)}FOR {match.group(3)} IN {match.group(2)}",
        text,
    )
    rewritten = [translate_query_line(line) for line in text.splitlines()]
    result: list[str] = []
    with_ordinal = 0
    for line in rewritten:
        if re.match(r"^\s*WITH\b", line, re.I):
            with_ordinal += 1
            result.extend(translate_with_line(line, with_ordinal))
        else:
            result.append(line)
    return result


def query_blocks(lines: list[str]) -> list[str]:
    blocks: list[str] = []
    current: list[str] | None = None
    for line in lines:
        if line.strip() == '"""':
            if current is None:
                current = []
            else:
                blocks.append("\n".join(current))
                current = None
            continue
        if current is not None:
            current.append(line)
    if current is not None:
        raise ValueError("unterminated Gherkin doc string")
    return blocks


def scenario_blockers(text: str) -> list[str]:
    blockers: list[str] = []
    checks = [
        (r"\bINSERT\b", "insert"),
        (r"\b__gql_with_scope_", "with_scope_translation"),
        (r"\bFOR\b", "for_iteration"),
        (r"\bCALL\b", "procedures"),
        (r"\bYIELD\b", "yield"),
        (r"\bUNION\b|\bEXCEPT\b|\bINTERSECT\b", "set_composition"),
        (r"\bMERGE\b", "merge_compatibility"),
        (r"\bON\s+(?:CREATE|MATCH)\b", "merge_actions"),
        (r"\bRETURN\s+\*", "return_star"),
        (r"\$[A-Za-z_]", "parameters"),
        (r"\[[^\]]*\bIN\b[^\]]*\|", "list_comprehension"),
    ]
    for pattern, blocker in checks:
        if re.search(pattern, text, re.I | re.S):
            blockers.append(blocker)
    if re.search(r"\bMERGE\b[^\n]*(?:--|<-|->|\[)", text, re.I):
        blockers.append("merge_path")
    return blockers


def transform_feature(source: Path, relative: Path) -> tuple[str, list[Scenario]]:
    source_lines = source.read_text(encoding="utf-8").splitlines()
    transformed: list[str] = []
    query: list[str] | None = None
    notice_added = False
    for line in source_lines:
        if line.strip() == '"""':
            if query is None:
                query = []
            else:
                transformed.extend(translate_query_block(query))
                query = None
            transformed.append(line)
            continue
        if query is not None:
            query.append(line)
            continue
        transformed.append(line)
        if not notice_added and line.strip().lower() == "#encoding: utf-8":
            transformed.extend(
                [
                    "#",
                    f"# Source: {UPSTREAM_URL}/blob/{PINNED_REVISION}/tck/features/clauses/{relative.as_posix()}",
                    MODIFICATION_NOTICE,
                ]
            )
            notice_added = True
    if query is not None:
        raise ValueError(f"unterminated query doc string in {relative}")
    if not notice_added:
        raise ValueError(f"missing encoding marker in {relative}")

    scenarios: list[Scenario] = []
    source_starts: list[tuple[int, str]] = []
    for index, line in enumerate(source_lines):
        match = re.match(r"^\s*Scenario(?: Outline)?:\s*(.+)$", line)
        if match:
            source_starts.append((index, match.group(1).strip()))
    target_starts: list[tuple[int, str]] = []
    for index, line in enumerate(transformed):
        match = re.match(r"^\s*Scenario(?: Outline)?:\s*(.+)$", line)
        if match:
            target_starts.append((index, match.group(1).strip()))
    if [name for _, name in source_starts] != [name for _, name in target_starts]:
        raise ValueError(f"scenario declarations changed during translation: {relative}")
    target_path = relative.as_posix()
    for scenario_index, (start, name) in enumerate(target_starts):
        end = (
            target_starts[scenario_index + 1][0]
            if scenario_index + 1 < len(target_starts)
            else len(transformed)
        )
        scenarios.append(
            Scenario(
                source_path=f"tck/features/clauses/{relative.as_posix()}",
                target_path=target_path,
                source_line=source_starts[scenario_index][0] + 1,
                name=name,
                text="\n".join(transformed[start:end]),
            )
        )
    return "\n".join(transformed) + "\n", scenarios


def write_manifest(path: Path, scenarios: list[Scenario]) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, delimiter="\t", lineterminator="\n")
        writer.writerow(
            [
                "source_path",
                "source_line",
                "scenario",
                "target_path",
                "status",
                "blockers",
            ]
        )
        for scenario in scenarios:
            writer.writerow(
                [
                    scenario.source_path,
                    scenario.source_line,
                    scenario.name,
                    scenario.target_path,
                    "ported_unverified",
                    ",".join(scenario_blockers(scenario.text)) or "semantic_review",
                ]
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True, help="openCypher repository checkout")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("test/features/clauses"),
        help="generated clause corpus directory",
    )
    parser.add_argument(
        "--allow-revision",
        action="store_true",
        help="permit a source revision other than the reviewed pin",
    )
    args = parser.parse_args()
    source = args.source.resolve()
    clauses = source / "tck" / "features" / "clauses"
    if not clauses.is_dir():
        raise SystemExit(f"missing upstream clause directory: {clauses}")
    revision = git_revision(source)
    if revision != PINNED_REVISION and not args.allow_revision:
        raise SystemExit(
            f"source revision {revision} differs from reviewed pin {PINNED_REVISION}; "
            "review the upstream delta, then use --allow-revision and update the pin"
        )

    output = args.output.resolve()
    repository = Path(__file__).resolve().parents[1]
    expected_parent = (repository / "test" / "features").resolve()
    if output.parent != expected_parent or output.name != "clauses":
        raise SystemExit(f"refusing to replace unexpected output directory: {output}")
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    scenarios: list[Scenario] = []
    feature_files = sorted(clauses.rglob("*.feature"))
    for feature in feature_files:
        relative = feature.relative_to(clauses)
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        content, feature_scenarios = transform_feature(feature, relative)
        target.write_text(content, encoding="utf-8")
        scenarios.extend(feature_scenarios)

    shutil.copyfile(source / "LICENSE", output / "LICENSE.openCypher")
    shutil.copyfile(source / "NOTICE", output / "NOTICE.openCypher")
    write_manifest(output / "manifest.tsv", scenarios)
    metadata = {
        "source": UPSTREAM_URL,
        "revision": revision,
        "source_directory": "tck/features/clauses",
        "feature_files": len(feature_files),
        "scenario_declarations": len(scenarios),
        "query_transforms": [
            "clause CREATE to INSERT, except ON CREATE",
            "UNWIND expression AS variable to FOR variable IN expression",
            "SKIP to OFFSET",
            "id(expression) to element_id(expression)",
            "Cypher WITH projection to syntax-only GQL LET bindings with semantic-review markers",
        ],
        "execution_status": "ported_unverified",
    }
    (output / "SOURCE.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"ported {len(feature_files)} feature files and {len(scenarios)} scenario declarations from {revision}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
