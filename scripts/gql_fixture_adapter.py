#!/usr/bin/env python3
"""Compile literal GQL INSERT setup patterns into COPY GRAPH SQLLogic fixtures.

This adapter intentionally handles only deterministic fixture data: literal
nodes, literal directed edges, scalar or scalar-list properties, and variable
references created within the same setup block. Query-dependent expressions,
loops, multiple labels, and dynamically mixed property types remain explicit
review gaps.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass, field
from pathlib import Path


class FixtureAdaptationError(ValueError):
    """The source setup cannot be represented by the current native fixture."""


@dataclass(frozen=True)
class Token:
    kind: str
    text: str
    offset: int


@dataclass(frozen=True)
class Literal:
    value: object
    kind: str


@dataclass
class Node:
    external_id: str
    label: str = ""
    properties: dict[str, Literal] = field(default_factory=dict)


@dataclass
class Edge:
    source_id: str
    target_id: str
    relationship_type: str
    properties: dict[str, Literal] = field(default_factory=dict)


@dataclass
class Fixture:
    nodes: list[Node]
    edges: list[Edge]


TOKEN = re.compile(
    r"""
    (?P<SPACE>\s+)
  | (?P<COMMENT>//[^\n]*)
  | (?P<STRING>'(?:''|\\.|[^'])*'|"(?:""|\\.|[^"])*")
  | (?P<NUMBER>(?:\d+\.\d*|\.\d+|\d+)(?:[eE][+-]?\d+)?)
  | (?P<ARROW_IN><-)
  | (?P<ARROW_OUT>->)
  | (?P<IDENT>`(?:``|[^`])+`|[A-Za-z_][A-Za-z0-9_]*)
  | (?P<PUNCT>[()\[\]{},:;.+-])
    """,
    re.VERBOSE,
)


def tokenize(text: str) -> list[Token]:
    result: list[Token] = []
    offset = 0
    while offset < len(text):
        match = TOKEN.match(text, offset)
        if not match:
            excerpt = text[offset : offset + 32].splitlines()[0]
            raise FixtureAdaptationError(
                f"unsupported token at offset {offset}: {excerpt!r}"
            )
        kind = match.lastgroup or ""
        if kind not in {"SPACE", "COMMENT"}:
            result.append(Token(kind, match.group(), offset))
        offset = match.end()
    result.append(Token("EOF", "", len(text)))
    return result


def identifier(token: Token) -> str:
    if token.kind != "IDENT":
        raise FixtureAdaptationError(
            f"expected identifier at offset {token.offset}, found {token.text!r}"
        )
    if token.text.startswith("`"):
        return token.text[1:-1].replace("``", "`")
    return token.text


class Parser:
    def __init__(self, text: str):
        self.tokens = tokenize(text)
        self.position = 0
        self.nodes: list[Node] = []
        self.edges: list[Edge] = []
        self.variables: dict[str, str] = {}

    def current(self) -> Token:
        return self.tokens[self.position]

    def advance(self) -> Token:
        token = self.current()
        self.position += 1
        return token

    def accepts(self, text: str) -> bool:
        if self.current().text.upper() == text.upper():
            self.advance()
            return True
        return False

    def expect(self, text: str) -> Token:
        if not self.accepts(text):
            token = self.current()
            raise FixtureAdaptationError(
                f"expected {text!r} at offset {token.offset}, found {token.text!r}"
            )
        return self.tokens[self.position - 1]

    def expect_identifier(self) -> str:
        return identifier(self.advance())

    def parse(self) -> Fixture:
        if self.current().kind == "EOF":
            return Fixture([], [])
        while self.current().kind != "EOF":
            self.expect("INSERT")
            self.parse_pattern()
            while self.accepts(","):
                self.parse_pattern()
            self.accepts(";")
            if self.current().kind != "EOF" and self.current().text.upper() != "INSERT":
                token = self.current()
                raise FixtureAdaptationError(
                    f"fixture contains non-INSERT clause {token.text!r} at offset "
                    f"{token.offset}"
                )
        return Fixture(self.nodes, self.edges)

    def parse_pattern(self) -> None:
        left = self.parse_node()
        while self.current().text in {"-", "<-"}:
            incoming = self.accepts("<-")
            if not incoming:
                self.expect("-")
            relationship_type, properties = self.parse_edge()
            if incoming:
                self.expect("-")
            else:
                if not self.accepts("->"):
                    raise FixtureAdaptationError(
                        "undirected INSERT relationships cannot be represented exactly"
                    )
            right = self.parse_node()
            source, target = (right, left) if incoming else (left, right)
            self.edges.append(Edge(source, target, relationship_type, properties))
            left = right

    def parse_node(self) -> str:
        self.expect("(")
        variable = ""
        if self.current().kind == "IDENT":
            variable = self.expect_identifier()
        labels: list[str] = []
        while self.accepts(":"):
            labels.append(self.expect_identifier())
        properties = self.parse_map() if self.current().text == "{" else {}
        self.expect(")")
        if variable and variable in self.variables:
            if labels or properties:
                raise FixtureAdaptationError(
                    f"bound node {variable!r} is redeclared with labels or properties"
                )
            return self.variables[variable]
        external_id = f"n{len(self.nodes) + 1}"
        self.nodes.append(Node(external_id, ";".join(labels), properties))
        if variable:
            self.variables[variable] = external_id
        return external_id

    def parse_edge(self) -> tuple[str, dict[str, Literal]]:
        self.expect("[")
        if self.current().kind == "IDENT":
            self.expect_identifier()  # Edge variable is irrelevant to fixture identity.
        self.expect(":")
        relationship_type = self.expect_identifier()
        properties = self.parse_map() if self.current().text == "{" else {}
        self.expect("]")
        return relationship_type, properties

    def parse_map(self) -> dict[str, Literal]:
        result: dict[str, Literal] = {}
        self.expect("{")
        if self.accepts("}"):
            return result
        while True:
            key = self.expect_identifier()
            self.expect(":")
            if key.casefold() in {existing.casefold() for existing in result}:
                raise FixtureAdaptationError(f"duplicate property {key!r}")
            result[key] = self.parse_literal()
            if self.accepts("}"):
                break
            self.expect(",")
        return result

    def parse_literal(self) -> Literal:
        negative = self.accepts("-")
        token = self.advance()
        if token.text == "[" and not negative:
            values: list[Literal] = []
            if not self.accepts("]"):
                while True:
                    value = self.parse_literal()
                    if value.kind.startswith("list:"):
                        raise FixtureAdaptationError("nested list properties are not supported")
                    if value.kind == "null":
                        raise FixtureAdaptationError(
                            "NULL elements in list properties are not supported"
                        )
                    if value.kind == "string" and ";" in str(value.value):
                        raise FixtureAdaptationError(
                            "string list elements containing ';' are not supported"
                        )
                    values.append(value)
                    if self.accepts("]"):
                        break
                    self.expect(",")
            kinds = {value.kind for value in values}
            if kinds <= {"int", "double"} and "double" in kinds:
                element_kind = "double"
            elif len(kinds) == 1:
                element_kind = next(iter(kinds))
            elif not kinds:
                element_kind = "unknown"
            else:
                raise FixtureAdaptationError(
                    "list property contains incompatible literal types: "
                    + ", ".join(sorted(kinds))
                )
            return Literal(tuple(values), f"list:{element_kind}")
        if token.kind == "NUMBER":
            if any(character in token.text.lower() for character in {".", "e"}):
                value: object = float(token.text)
                kind = "double"
            else:
                value = int(token.text)
                kind = "int"
            if negative:
                value = -value  # type: ignore[operator]
            return Literal(value, kind)
        if negative:
            raise FixtureAdaptationError(
                f"unary minus requires a numeric literal at offset {token.offset}"
            )
        if token.kind == "STRING":
            quote = token.text[0]
            body = token.text[1:-1].replace(quote + quote, quote)
            body = body.replace("\\" + quote, quote).replace("\\\\", "\\")
            return Literal(body, "string")
        if token.kind == "IDENT":
            keyword = token.text.casefold()
            if keyword == "true":
                return Literal(True, "bool")
            if keyword == "false":
                return Literal(False, "bool")
            if keyword == "null":
                return Literal(None, "null")
            if keyword in {"date", "localtime", "localdatetime"} and self.current().text == "(":
                return self.parse_temporal_literal(keyword)
            if self.accepts("."):
                property_name = self.expect_identifier()
                external_id = self.variables.get(identifier(token))
                if external_id is None:
                    raise FixtureAdaptationError(
                        f"property reference uses unbound variable {token.text!r}"
                    )
                node = next(
                    node for node in self.nodes if node.external_id == external_id
                )
                value = next(
                    (
                        value
                        for name, value in node.properties.items()
                        if name.casefold() == property_name.casefold()
                    ),
                    None,
                )
                if value is None:
                    raise FixtureAdaptationError(
                        f"bound node {token.text!r} has no property {property_name!r}"
                    )
                return value
        raise FixtureAdaptationError(
            f"property values must be scalar literals; found {token.text!r} at "
            f"offset {token.offset}"
        )

    def parse_temporal_literal(self, kind: str) -> Literal:
        self.expect("(")
        fields = self.parse_map()
        self.expect(")")

        def integer(name: str, default: int | None = None) -> int:
            value = next(
                (value for key, value in fields.items() if key.casefold() == name),
                None,
            )
            if value is None:
                if default is None:
                    raise FixtureAdaptationError(
                        f"{kind} constructor requires {name!r}"
                    )
                return default
            if value.kind != "int":
                raise FixtureAdaptationError(
                    f"{kind} constructor field {name!r} must be an integer literal"
                )
            assert isinstance(value.value, int)
            return value.value

        allowed = {
            "date": {"year", "month", "day"},
            "localtime": {"hour", "minute", "second", "nanosecond"},
            "localdatetime": {
                "year",
                "month",
                "day",
                "hour",
                "minute",
                "second",
                "nanosecond",
            },
        }[kind]
        unknown = sorted(key for key in fields if key.casefold() not in allowed)
        if unknown:
            raise FixtureAdaptationError(
                f"{kind} constructor has unsupported fields: {', '.join(unknown)}"
            )
        if kind == "date":
            value = f"{integer('year'):04d}-{integer('month'):02d}-{integer('day'):02d}"
            return Literal(value, "date")
        hour = integer("hour")
        minute = integer("minute")
        second = integer("second", 0)
        nanosecond = integer("nanosecond", 0)
        if not 0 <= nanosecond <= 999_999_999:
            raise FixtureAdaptationError("nanosecond must be between 0 and 999999999")
        time = f"{hour:02d}:{minute:02d}:{second:02d}"
        if nanosecond:
            time += f".{nanosecond:09d}"
        if kind == "localtime":
            return Literal(time, "localtime")
        value = (
            f"{integer('year'):04d}-{integer('month'):02d}-{integer('day'):02d}T"
            + time
        )
        return Literal(value, "localdatetime")


def parse_insert_fixture(text: str) -> Fixture:
    return Parser(expand_for_prefix(text)).parse()


FOR_PREFIX = re.compile(
    r"^\s*FOR\s+([A-Za-z_][A-Za-z0-9_]*)\s+IN\s+"
    r"(?:range\(\s*(-?\d+)\s*,\s*(-?\d+)(?:\s*,\s*(-?\d+))?\s*\)"
    r"|\[\s*([^\]]*)\s*\])\s*(?:\n|\r\n?)(.*)$",
    re.I | re.S,
)


def expand_for_prefix(text: str) -> str:
    """Unroll deterministic prefix FOR loops used by source fixture setup."""
    match = FOR_PREFIX.match(text)
    if not match:
        return text
    variable, start, end, step, literal_list, body = match.groups()
    if literal_list is not None:
        values = []
        for item in literal_list.split(","):
            item = item.strip()
            if not re.fullmatch(r"-?\d+", item):
                raise FixtureAdaptationError(
                    "fixture FOR lists currently require integer literals"
                )
            values.append(int(item))
    else:
        assert start is not None and end is not None
        stride = int(step) if step is not None else 1
        if stride == 0:
            raise FixtureAdaptationError("fixture FOR range step cannot be zero")
        stop = int(end) + (1 if stride > 0 else -1)
        values = list(range(int(start), stop, stride))
    if len(values) > 100_000:
        raise FixtureAdaptationError("fixture FOR expansion exceeds 100000 rows")
    result = []
    # A matching identifier followed by ':' is a property key or label/type,
    # not the loop-variable expression on the right-hand side.
    pattern = re.compile(rf"\b{re.escape(variable)}\b(?!\s*:)")
    for value in values:
        result.append(expand_for_prefix(pattern.sub(str(value), body)))
    return "\n".join(result)


SQL_TYPES = {
    "bool": "BOOLEAN",
    "int": "BIGINT",
    "double": "DOUBLE",
    "string": "VARCHAR",
    "date": "VARCHAR",
    "localtime": "VARCHAR",
    "localdatetime": "VARCHAR",
    "variant": "VARCHAR",
}

HEADER_TYPES = {
    "bool": "boolean",
    "int": "long",
    "double": "double",
    "string": "string",
    "date": "date",
    "localtime": "localtime",
    "localdatetime": "localdatetime",
    "variant": "variant",
}


def header_type(kind: str) -> str:
    if kind.startswith("list:"):
        element_kind = kind.split(":", 1)[1]
        return HEADER_TYPES[element_kind] + "[]"
    return HEADER_TYPES[kind]


def property_schema(elements: list[Node] | list[Edge]) -> list[tuple[str, str]]:
    property_kinds: dict[str, set[str]] = {}
    original_names: dict[str, str] = {}
    for element in elements:
        for name, value in element.properties.items():
            normalized = name.casefold()
            original_names.setdefault(normalized, name)
            if value.kind != "null":
                property_kinds.setdefault(normalized, set()).add(value.kind)
    result: list[tuple[str, str]] = []
    for normalized in sorted(original_names):
        kinds = property_kinds.get(normalized, set())
        list_kinds = {kind for kind in kinds if kind.startswith("list:")}
        if list_kinds:
            if len(list_kinds) != len(kinds):
                raise FixtureAdaptationError(
                    f"property {original_names[normalized]!r} mixes list and scalar values"
                )
            element_kinds = {kind.split(":", 1)[1] for kind in list_kinds}
            element_kinds.discard("unknown")
            if element_kinds <= {"int", "double"} and "double" in element_kinds:
                kind = "list:double"
            elif len(element_kinds) == 1:
                kind = "list:" + next(iter(element_kinds))
            elif not element_kinds:
                kind = "list:string"
            else:
                raise FixtureAdaptationError(
                    f"property {original_names[normalized]!r} has incompatible list "
                    f"element types: {', '.join(sorted(element_kinds))}"
                )
        elif kinds <= {"int", "double"} and "double" in kinds:
            kind = "double"
        elif len(kinds) == 1:
            kind = next(iter(kinds))
        elif not kinds:
            raise FixtureAdaptationError(
                f"property {original_names[normalized]!r} contains only NULL values"
            )
        else:
            if kinds <= {"bool", "int", "double", "string"}:
                kind = "variant"
            else:
                raise FixtureAdaptationError(
                    f"property {original_names[normalized]!r} has incompatible literal "
                    f"types: {', '.join(sorted(kinds))}"
                )
        result.append((original_names[normalized], kind))
    return result


def quote_sql_string(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def quote_identifier(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def literal_sql(value: Literal | None, kind: str) -> str:
    if value is None or value.kind == "null":
        source_type = "VARCHAR" if kind.startswith("list:") else SQL_TYPES[kind]
        return f"CAST(NULL AS {source_type})"
    if kind.startswith("list:"):
        values = value.value
        assert isinstance(values, tuple)
        serialized = ";".join(
            str(item.value).lower() if item.kind == "bool" else str(item.value)
            for item in values
        )
        return quote_sql_string(serialized)
    if kind == "variant":
        prefixes = {"bool": "b:", "int": "i:", "double": "d:", "string": "s:"}
        if value.kind not in prefixes:
            raise FixtureAdaptationError(
                f"VARIANT fixture encoding does not support {value.kind!r}"
            )
        payload = (
            str(value.value).lower() if value.kind == "bool" else str(value.value)
        )
        return quote_sql_string(prefixes[value.kind] + payload)
    if value.kind in {"string", "date", "localtime", "localdatetime"}:
        return quote_sql_string(str(value.value))
    if value.kind == "bool":
        return "TRUE" if value.value else "FALSE"
    return repr(value.value)


def values_copy_sql(
    rows: list[list[str]], headers: list[str], output_path: str
) -> str:
    aliases = ", ".join(quote_identifier(header) for header in headers)
    if rows:
        values = ",\n        ".join("(" + ", ".join(row) + ")" for row in rows)
        source = f"SELECT * FROM (VALUES\n        {values}\n    ) fixture({aliases})"
    else:
        projections = ", ".join(
            f"CAST(NULL AS VARCHAR) AS {quote_identifier(header)}" for header in headers
        )
        source = f"SELECT {projections} WHERE FALSE"
    return (
        "statement ok\n"
        f"COPY (\n    {source}\n) TO {quote_sql_string(output_path)} "
        "(FORMAT CSV, HEADER)"
    )


def safe_name(value: str) -> str:
    result = re.sub(r"[^A-Za-z0-9_]", "_", value).strip("_").lower()
    if not result or not result[0].isalpha():
        result = "fixture_" + result
    return result


def compile_fixture_sql(text: str, fixture_name: str) -> str:
    fixture = parse_insert_fixture(text)
    name = safe_name(fixture_name)
    node_properties = property_schema(fixture.nodes)
    edge_properties = property_schema(fixture.edges)
    group = f"GqlFixture_{name}"
    node_headers = [f":ID({group})"] + [
        f"{property}:{header_type(kind)}" for property, kind in node_properties
    ] + [":LABEL"]
    edge_headers = [f":START_ID({group})", f":END_ID({group})", ":TYPE"] + [
        f"{property}:{header_type(kind)}" for property, kind in edge_properties
    ]
    node_rows = []
    for node in fixture.nodes:
        normalized = {key.casefold(): value for key, value in node.properties.items()}
        node_rows.append(
            [quote_sql_string(node.external_id)]
            + [
                literal_sql(normalized.get(property.casefold()), kind)
                for property, kind in node_properties
            ]
            + [quote_sql_string(node.label)]
        )
    edge_rows = []
    for edge in fixture.edges:
        normalized = {key.casefold(): value for key, value in edge.properties.items()}
        edge_rows.append(
            [
                quote_sql_string(edge.source_id),
                quote_sql_string(edge.target_id),
                quote_sql_string(edge.relationship_type),
            ]
            + [
                literal_sql(normalized.get(property.casefold()), kind)
                for property, kind in edge_properties
            ]
        )
    node_path = f"__TEST_DIR__/{name}_nodes.csv"
    edge_path = f"__TEST_DIR__/{name}_edges.csv"
    return "\n\n".join(
        [
            values_copy_sql(node_rows, node_headers, node_path),
            values_copy_sql(edge_rows, edge_headers, edge_path),
            f"statement ok\nCREATE GRAPH {name} ANY",
            (
                f"statement ok\nCOPY GRAPH {name} FROM (\n"
                f"    VERTICES {quote_sql_string(node_path)},\n"
                f"    EDGES {quote_sql_string(edge_path)}\n"
                ") FORMAT NEO4J"
            ),
            f"statement ok\nSESSION SET GRAPH {name}",
        ]
    )


def self_test() -> None:
    source = """
    INSERT (a:Person {name: 'Alice', age: 42}),
           (b:Person {name: 'Bob', scores: [1, 2, 3], mixed: 42}),
           (a)-[:KNOWS {since: 2020}]->(b),
           (:City {name: 'Paris', founded: a.age, mixed: 'forty-two'})
    """
    fixture = parse_insert_fixture(source)
    assert len(fixture.nodes) == 3
    assert len(fixture.edges) == 1
    assert fixture.edges[0].source_id == "n1"
    assert fixture.edges[0].target_id == "n2"
    output = compile_fixture_sql(source, "fixture_adapter_smoke")
    assert "CREATE GRAPH fixture_adapter_smoke ANY" in output
    assert '"age:long"' in output
    assert '"scores:long[]"' in output
    assert '"mixed:variant"' in output
    assert "CAST(NULL AS BIGINT)" in output
    assert "'1;2;3'" in output
    assert "'i:42'" in output and "'s:forty-two'" in output
    assert "'KNOWS'" in output
    loop_fixture = parse_insert_fixture(
        "FOR i IN range(1, 3)\nINSERT ({nr: i})"
    )
    assert [node.properties["nr"].value for node in loop_fixture.nodes] == [1, 2, 3]
    print("fixture adapter self-test passed")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", nargs="?", type=Path)
    parser.add_argument("--name", default="gql_feature_fixture")
    parser.add_argument("--self-test", action="store_true")
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
        return 0
    if arguments.input is None:
        parser.error("input is required unless --self-test is used")
    print(compile_fixture_sql(arguments.input.read_text(encoding="utf-8"), arguments.name))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
