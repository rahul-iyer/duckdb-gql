#!/usr/bin/env python3
"""Generate runnable SQLLogic candidates for semantically simple scenarios.

Candidates are deliberately written outside ``test/sql``. They must pass the
DuckDB SQLLogic runner and receive a source-semantic review before promotion to
``test/sql`` and ``test/features/execution.tsv``.
"""

from __future__ import annotations

import argparse
import csv
import re
import shutil
from pathlib import Path

from generate_gql_clause_sqllogic import (
    background_text,
    having_executed_blocks,
    scenario_blocks,
)
from gql_fixture_adapter import FixtureAdaptationError, compile_fixture_sql, safe_name


ROOT = Path(__file__).resolve().parents[1]
CORPUS = ROOT / "test" / "features" / "clauses"
FEATURE_ROOT = ROOT / "test" / "features"


def load_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        return list(csv.DictReader(source, delimiter="\t"))


def doc_string_after(text: str, step: str) -> str | None:
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if step.casefold() not in line.casefold():
            continue
        index += 1
        while index < len(lines) and lines[index].strip() != '"""':
            index += 1
        if index == len(lines):
            return None
        start = index + 1
        index = start
        while index < len(lines) and lines[index].strip() != '"""':
            index += 1
        if index == len(lines):
            return None
        return "\n".join(line.strip() for line in lines[start:index]).strip()
    return None


def result_table(text: str) -> tuple[list[str], list[list[str]], bool] | None:
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if "then the result should be" not in line.casefold():
            continue
        rowsort = "any order" in line.casefold()
        index += 1
        table_lines: list[str] = []
        while index < len(lines):
            stripped = lines[index].strip()
            if stripped.startswith("|") and stripped.endswith("|"):
                table_lines.append(stripped)
                index += 1
                continue
            if table_lines:
                break
            index += 1
        if not table_lines:
            return None
        parsed = [
            [cell.strip() for cell in row[1:-1].split("|")] for row in table_lines
        ]
        return parsed[0], parsed[1:], rowsort
    return None


def scalar_cell(cell: str) -> tuple[str, str] | None:
    if cell.casefold() == "null":
        return "NULL", "null"
    if re.fullmatch(r"-?\d+", cell):
        return cell, "int"
    if re.fullmatch(r"-?(?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?", cell):
        return cell, "real"
    if cell.casefold() in {"true", "false"}:
        return cell.casefold(), "text"
    if len(cell) >= 2 and cell[0] == cell[-1] == "'":
        return cell[1:-1].replace("''", "'"), "text"
    return None


def expected_result(rows: list[list[str]], width: int) -> tuple[str, list[str]] | None:
    if not rows or any(len(row) != width for row in rows):
        return None
    converted: list[list[tuple[str, str]]] = []
    for row in rows:
        converted_row = []
        for cell in row:
            value = scalar_cell(cell)
            if value is None:
                return None
            converted_row.append(value)
        converted.append(converted_row)
    types = []
    for column in range(width):
        kinds = {
            row[column][1] for row in converted if row[column][1] != "null"
        }
        if kinds == {"int"}:
            types.append("I")
        elif kinds <= {"int", "real"} and "real" in kinds:
            types.append("R")
        else:
            types.append("T")
    return "".join(types), ["\t".join(value for value, _ in row) for row in converted]


def safe_query(query: str) -> bool:
    disallowed = re.compile(
        r"(?im)^\s*(?:INSERT|SET|REMOVE|DELETE|DETACH|MERGE|CALL|LET|FOR|UNION|EXCEPT|INTERSECT)\b"
    )
    if disallowed.search(query):
        return False
    if re.search(r":[A-Za-z_][A-Za-z0-9_]*:[A-Za-z_]", query):
        return False
    return bool(re.match(r"(?is)^\s*(?:OPTIONAL\s+)?MATCH\b", query))


def translate_vlp(query: str) -> str | None:
    """Translate openCypher relationship quantifiers to ISO GQL spelling."""

    # Use the explicit ISO edge-pattern spelling so intermediate variables in
    # openCypher's simplified arrows remain visible to the typed AST.
    query = query.replace("-->", "-[]->").replace("<--", "<-[]-")

    # COUNT is reserved in ISO GQL. Preserve openCypher property and alias
    # spelling with a delimited identifier at the language boundary.
    query = re.sub(r"(?i)\.count\b", '."count"', query)
    query = re.sub(r"(?i)\bAS\s+count\b", 'AS "count"', query)

    # A common TCK fixture constrains the same source variable in a preceding
    # MATCH. Folding that label into the path keeps the query equivalent and
    # presents one recursive pattern to the current native lowering.
    query = re.sub(
        r"(?im)^\s*MATCH\s+\(([A-Za-z_]\w*)(:[A-Za-z_]\w*)\)\s*\n"
        r"\s*MATCH\s+\(\1\)",
        lambda match: f"MATCH ({match.group(1)}{match.group(2)})",
        query,
    )

    unsupported_zero_bound = False

    def replace(match: re.Match[str]) -> str:
        nonlocal unsupported_zero_bound
        filler = match.group("filler")
        lower = match.group("lower")
        upper = match.group("upper")
        fixed = match.group("fixed")
        if fixed is not None:
            if fixed == "0":
                unsupported_zero_bound = True
            quantifier = "{" + fixed + "}"
        elif lower is None and upper is None:
            # A bare openCypher star defaults to one or more relationships.
            quantifier = "+"
        elif lower == "" and upper == "":
            quantifier = "+"
        elif upper == "":
            if lower == "0":
                quantifier = "*"
            elif lower == "1":
                quantifier = "+"
            else:
                quantifier = "{" + lower + ",}"
        elif lower == "":
            quantifier = "{1," + upper + "}"
        else:
            if lower == "0":
                unsupported_zero_bound = True
            quantifier = "{" + lower + "," + upper + "}"
        return f"-[{filler}]->{quantifier}"

    pattern = re.compile(
        r"-\[(?P<filler>[^\]*]*?)\*(?:(?P<lower>\d*)\.\."
        r"(?P<upper>\d*)|(?P<fixed>\d+))?\]->"
    )
    query = pattern.sub(replace, query)
    if unsupported_zero_bound:
        return None
    return query


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    output = arguments.output.resolve()
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    fixture_rows = load_tsv(FEATURE_ROOT / "sqllogic" / "fixtures.tsv")
    fixture_ready = {
        (row["source_path"], row["source_line"])
        for row in fixture_rows
        if row["status"] == "ready"
    }
    execution = {
        (row["source_path"], row["source_line"])
        for row in load_tsv(FEATURE_ROOT / "execution.tsv")
        if row["test_path"] != "test/sql/gql_feature_compatibility.test"
    }
    manifest = load_tsv(CORPUS / "manifest.tsv")
    manifest_by_target: dict[str, list[dict[str, str]]] = {}
    for row in manifest:
        manifest_by_target.setdefault(row["target_path"], []).append(row)

    generated: list[list[str]] = []
    for target_path, scenario_rows in sorted(manifest_by_target.items()):
        feature_path = CORPUS / target_path
        blocks = scenario_blocks(feature_path)
        block_by_name = dict(blocks)
        background = having_executed_blocks(background_text(feature_path))
        for row in scenario_rows:
            key = (row["source_path"], row["source_line"])
            if key in execution:
                continue
            block = block_by_name[row["scenario"]]
            if re.match(r"\s*Scenario Outline:", block):
                continue
            query = doc_string_after(block, "when executing query:")
            result = result_table(block)
            if query is None or result is None:
                continue
            query = translate_vlp(query)
            if query is None or not safe_query(query):
                continue
            if "no side effects" not in block.casefold():
                continue
            headers, expected_rows, rowsort = result
            expected = expected_result(expected_rows, len(headers))
            if expected is None:
                continue
            setup_blocks = background + having_executed_blocks(block)
            if setup_blocks and key not in fixture_ready:
                continue
            name = safe_name(
                f"candidate_{Path(target_path).stem}_{row['source_line']}"
            )
            try:
                fixture_sql = compile_fixture_sql("\n".join(setup_blocks), name)
            except FixtureAdaptationError:
                continue
            type_string, output_rows = expected
            if rowsort:
                output_rows.sort()
            test_path = output / f"{name}.test"
            contents = [
                f"# name: {test_path}",
                f"# Source: {row['source_path']}:{row['source_line']} {row['scenario']}",
                "# Generated candidate; semantic review required before promotion.",
                "# group: [gql]",
                "",
                "require gql",
                "",
                fixture_sql,
                "",
                f"query {type_string}" + (" rowsort" if rowsort else ""),
                query,
                "----",
                *output_rows,
                "",
            ]
            test_path.write_text("\n".join(contents), encoding="utf-8")
            generated.append(
                [row["source_path"], row["source_line"], str(test_path)]
            )

    with (output / "manifest.tsv").open(
        "w", encoding="utf-8", newline=""
    ) as destination:
        writer = csv.writer(destination, delimiter="\t", lineterminator="\n")
        writer.writerow(["source_path", "source_line", "test_path"])
        writer.writerows(generated)
    print(f"generated {len(generated)} executable SQLLogic candidates in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
