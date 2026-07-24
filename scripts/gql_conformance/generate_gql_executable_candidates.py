#!/usr/bin/env python3
"""Generate runnable SQLLogic candidates for semantically simple scenarios.

Candidates are deliberately written outside ``test/sql``. They must pass the
DuckDB SQLLogic runner and receive a source-semantic review before promotion to
``test/sql`` and ``test/features/execution.tsv``.
"""

from __future__ import annotations

import argparse
import csv
import itertools
import re
import shutil
from pathlib import Path

from generate_gql_clause_sqllogic import (
    background_text,
    having_executed_blocks,
    scenario_blocks,
)
from gql_fixture_adapter import (
    Edge,
    Fixture,
    FixtureAdaptationError,
    Literal,
    Node,
    compile_fixture_sql,
    parse_insert_fixture,
    property_schema,
    safe_name,
)


ROOT = Path(__file__).resolve().parents[2]
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


def normalized_properties(element: Node | Edge) -> dict[str, Literal]:
    return {name.casefold(): value for name, value in element.properties.items()}


def same_element(expected: Node | Edge, actual: Node | Edge) -> bool:
    if isinstance(expected, Node) != isinstance(actual, Node):
        return False
    if isinstance(expected, Node):
        assert isinstance(actual, Node)
        if expected.label.casefold() != actual.label.casefold():
            return False
    else:
        assert isinstance(expected, Edge) and isinstance(actual, Edge)
        if expected.relationship_type.casefold() != actual.relationship_type.casefold():
            return False
    return normalized_properties(expected) == normalized_properties(actual)


def render_literal(value: Literal | None) -> str:
    if value is None or value.kind == "null":
        return "NULL"
    if value.kind == "bool":
        return "true" if value.value else "false"
    if value.kind.startswith("list:"):
        assert isinstance(value.value, tuple)
        return "[" + ", ".join(render_literal(item) for item in value.value) + "]"
    return str(value.value)


def render_properties(
    element: Node | Edge, schema: list[tuple[str, str]]
) -> list[tuple[str, str]]:
    properties = normalized_properties(element)
    return [
        (name.casefold(), render_literal(properties.get(name.casefold())))
        for name, _ in schema
    ]


def render_node(node: Node, node_index: int, fixture: Fixture) -> str:
    fields = [
        ("__gql_id", str(node_index + 1)),
        (
            "__gql_labels",
            ";".join(label.strip().casefold() for label in node.label.split(";")),
        ),
        *render_properties(node, property_schema(fixture.nodes)),
    ]
    return "{" + ", ".join(f"'{name}': {value}" for name, value in fields) + "}"


def render_edge(edge: Edge, edge_index: int, fixture: Fixture) -> str:
    node_indices = {node.external_id: index + 1 for index, node in enumerate(fixture.nodes)}
    fields = [
        ("__gql_id", str(edge_index + 1)),
        ("__gql_type", edge.relationship_type.casefold()),
        ("__gql_source", str(node_indices[edge.source_id])),
        ("__gql_target", str(node_indices[edge.target_id])),
        *render_properties(edge, property_schema(fixture.edges)),
    ]
    return "{" + ", ".join(f"'{name}': {value}" for name, value in fields) + "}"


def node_candidates(expected: Node, fixture: Fixture) -> list[int]:
    return [
        index
        for index, node in enumerate(fixture.nodes)
        if same_element(expected, node)
    ]


def edge_candidates(expected: Edge, fixture: Fixture) -> list[int]:
    return [
        index
        for index, edge in enumerate(fixture.edges)
        if same_element(expected, edge)
    ]


def graph_cell(cell: str, fixture: Fixture | None) -> tuple[str, str] | None:
    if fixture is None:
        return None
    try:
        if cell.startswith("(") and cell.endswith(")"):
            expected = parse_insert_fixture("INSERT " + cell)
            if len(expected.nodes) != 1 or expected.edges:
                return None
            candidates = node_candidates(expected.nodes[0], fixture)
            if len(candidates) != 1:
                return None
            index = candidates[0]
            return render_node(fixture.nodes[index], index, fixture), "text"

        if cell.startswith("[:") and cell.endswith("]"):
            expected = parse_insert_fixture("INSERT ()-" + cell + "->()")
            if len(expected.edges) != 1:
                return None
            candidates = edge_candidates(expected.edges[0], fixture)
            if len(candidates) != 1:
                return None
            index = candidates[0]
            return render_edge(fixture.edges[index], index, fixture), "text"

        if cell.startswith("<") and cell.endswith(")>"):
            expected = parse_insert_fixture("INSERT " + cell[1:-1])
            if not expected.nodes:
                return None
            candidate_sets = [node_candidates(node, fixture) for node in expected.nodes]
            if any(not candidates for candidates in candidate_sets):
                return None
            local_positions = {
                node.external_id: index for index, node in enumerate(expected.nodes)
            }
            matches: list[tuple[tuple[int, ...], tuple[int, ...]]] = []
            for node_choice in itertools.product(*candidate_sets):
                edge_choice: list[int] = []
                valid = True
                for edge in expected.edges:
                    source = fixture.nodes[node_choice[local_positions[edge.source_id]]]
                    target = fixture.nodes[node_choice[local_positions[edge.target_id]]]
                    candidates = [
                        index
                        for index, actual in enumerate(fixture.edges)
                        if actual.source_id == source.external_id
                        and actual.target_id == target.external_id
                        and same_element(edge, actual)
                    ]
                    if len(candidates) != 1:
                        valid = False
                        break
                    edge_choice.append(candidates[0])
                if valid:
                    matches.append((node_choice, tuple(edge_choice)))
            if len(matches) != 1:
                return None
            node_choice, edge_choice = matches[0]
            nodes = ", ".join(
                render_node(fixture.nodes[index], index, fixture)
                for index in node_choice
            )
            edges = ", ".join(
                render_edge(fixture.edges[index], index, fixture)
                for index in edge_choice
            )
            return f"{{'nodes': [{nodes}], 'edges': [{edges}]}}", "text"
    except (FixtureAdaptationError, KeyError):
        return None
    return None


def result_cell(cell: str, fixture: Fixture | None) -> tuple[str, str] | None:
    return scalar_cell(cell) or graph_cell(cell, fixture)


def expected_result(
    rows: list[list[str]], width: int, fixture: Fixture | None = None
) -> tuple[str, list[str]] | None:
    if not rows or any(len(row) != width for row in rows):
        return None
    converted: list[list[tuple[str, str]]] = []
    for row in rows:
        converted_row = []
        for cell in row:
            value = result_cell(cell, fixture)
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


def self_test() -> None:
    fixture = parse_insert_fixture(
        "INSERT (a:A {name: 'Ada', age: 42}), "
        "(b:B {name: 'Grace'}), (a)-[:KNOWS {since: 2020}]->(b)"
    )
    assert graph_cell("(:A {name: 'Ada', age: 42})", fixture) == (
        "{'__gql_id': 1, '__gql_labels': a, 'age': 42, 'name': Ada}",
        "text",
    )
    assert graph_cell("[:KNOWS {since: 2020}]", fixture) == (
        "{'__gql_id': 1, '__gql_type': knows, '__gql_source': 1, "
        "'__gql_target': 2, 'since': 2020}",
        "text",
    )
    assert graph_cell(
        "<(:A {name: 'Ada', age: 42})-[:KNOWS {since: 2020}]->"
        "(:B {name: 'Grace'})>",
        fixture,
    ) == (
        "{'nodes': [{'__gql_id': 1, '__gql_labels': a, 'age': 42, 'name': Ada}, "
        "{'__gql_id': 2, '__gql_labels': b, 'age': NULL, 'name': Grace}], "
        "'edges': [{'__gql_id': 1, '__gql_type': knows, '__gql_source': 1, "
        "'__gql_target': 2, 'since': 2020}]}",
        "text",
    )
    duplicate = parse_insert_fixture("INSERT (:A), (:A)")
    assert graph_cell("(:A)", duplicate) is None
    print("graph result adapter self-test passed")


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
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
        return 0
    if arguments.output is None:
        parser.error("--output is required unless --self-test is used")
    output = arguments.output.resolve()
    try:
        output_display = output.relative_to(ROOT)
    except ValueError:
        output_display = output
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
            setup_blocks = background + having_executed_blocks(block)
            if setup_blocks and key not in fixture_ready:
                continue
            fixture_text = "\n".join(setup_blocks)
            try:
                fixture = parse_insert_fixture(fixture_text)
            except FixtureAdaptationError:
                continue
            headers, expected_rows, rowsort = result
            expected = expected_result(expected_rows, len(headers), fixture)
            if expected is None:
                continue
            name = safe_name(
                f"candidate_{Path(target_path).stem}_{row['source_line']}"
            )
            try:
                fixture_sql = compile_fixture_sql(fixture_text, name)
            except FixtureAdaptationError:
                continue
            type_string, output_rows = expected
            if rowsort:
                output_rows.sort()
            test_path = output / f"{name}.test"
            test_path_display = output_display / test_path.name
            contents = [
                f"# name: {test_path_display.as_posix()}",
                f"# Source: {row['source_path']}:{row['source_line']} {row['scenario']}",
                "# Generated candidate; semantic review required before promotion.",
                "# group: [gql]",
                "",
                "require duckgql",
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
                [row["source_path"], row["source_line"], test_path_display.as_posix()]
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
