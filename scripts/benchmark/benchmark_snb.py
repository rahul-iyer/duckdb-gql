#!/usr/bin/env python3
"""Benchmark focused DuckGQL projections of an official LDBC SNB dataset.

This harness deliberately does not report an official LDBC score. It uses:

* an all-message projection (Post and Comment vertices) to measure native
  VARCHAR[] label search;
* a Person/KNOWS projection to measure lookup, one-hop, and two-hop queries.

Both projections are built from the official CsvBasic files. Query parameters
come from the official substitution-parameter archive when it is available.
"""

from __future__ import annotations

import argparse
import csv
import json
import platform
import re
import statistics
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


PHASE_MARKER = "__DUCKGQL_SNB_PHASE__"
TIMER_RE = re.compile(r"Run Time \(s\): real ([0-9.]+)")


def sql_literal(value: Path | str) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def summarize(values: list[float]) -> dict[str, float]:
    return {
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
        "mean": statistics.mean(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def parse_timers(output: str) -> dict[str, float]:
    pending: str | None = None
    phases: dict[str, float] = {}
    for line in output.splitlines():
        if line.startswith(PHASE_MARKER):
            pending = line[len(PHASE_MARKER) :].strip()
            continue
        match = TIMER_RE.search(line)
        if match and pending is not None:
            phases[pending] = float(match.group(1))
            pending = None
    if pending is not None:
        raise RuntimeError(f"missing timer for phase {pending}")
    return phases


def run_cli(
    cli: Path,
    sql: str,
    database: Path | None = None,
    extension: Path | None = None,
) -> subprocess.CompletedProcess[str]:
    command = [str(cli), "-unsigned", "-no-init"]
    if database is not None:
        command += ["-storage-version", "v1.5.0", str(database)]
    if extension is not None:
        command += ["-cmd", f"LOAD {sql_literal(extension)};"]
    return subprocess.run(
        command,
        input=sql,
        text=True,
        capture_output=True,
        check=False,
    )


def marked(name: str, statement: str) -> str:
    return (
        f".print {PHASE_MARKER}{name}\n"
        f"{statement.rstrip().rstrip(';')};\n"
    )


def read_official_seed(parameters: Path | None) -> tuple[int, str]:
    fallback = (6597069812321, "repository fallback")
    if parameters is None:
        return fallback
    path = parameters / "interactive_1_param.txt"
    if not path.exists():
        return fallback
    with path.open(newline="") as source:
        row = next(csv.DictReader(source, delimiter="|"), None)
    if row is None or not row.get("personId"):
        return fallback
    return int(row["personId"]), str(path)


def prepare_inputs(
    cli: Path,
    dataset: Path,
    prepared: Path,
    threads: int,
    memory_limit: str,
) -> tuple[dict[str, float], dict[str, Path]]:
    dynamic = dataset / "dynamic"
    required_sources = {
        "person": dynamic / "person_0_0.csv",
        "post": dynamic / "post_0_0.csv",
        "comment": dynamic / "comment_0_0.csv",
        "knows": dynamic / "person_knows_person_0_0.csv",
    }
    missing = [str(path) for path in required_sources.values() if not path.exists()]
    if missing:
        raise RuntimeError(f"missing required CsvBasic files: {missing}")

    prepared.mkdir(parents=True, exist_ok=True)
    outputs = {
        "content_nodes": prepared / "content-nodes.parquet",
        "content_edges": prepared / "content-edges.parquet",
        "person_nodes": prepared / "person-nodes.parquet",
        "person_edges": prepared / "person-edges.parquet",
    }
    if all(path.exists() and path.stat().st_size > 0 for path in outputs.values()):
        return {}, outputs

    post = sql_literal(required_sources["post"])
    comment = sql_literal(required_sources["comment"])
    person = sql_literal(required_sources["person"])
    knows = sql_literal(required_sources["knows"])
    sql = f"""
PRAGMA threads={threads};
SET memory_limit={sql_literal(memory_limit)};
.timer on
"""
    if not outputs["content_nodes"].exists() or outputs["content_nodes"].stat().st_size == 0:
        sql += marked(
            "prepare_content_nodes",
            f"""
COPY (
    SELECT 'post:' || id::VARCHAR AS "external_id:ID(SNBContent)",
           id::BIGINT AS "entity_id:long",
           'Post;Message' AS ":LABEL"
    FROM read_csv({post}, delim='|', header=true, auto_detect=true)
    UNION ALL
    SELECT 'comment:' || id::VARCHAR AS "external_id:ID(SNBContent)",
           id::BIGINT AS "entity_id:long",
           'Comment;Message' AS ":LABEL"
    FROM read_csv({comment}, delim='|', header=true, auto_detect=true)
) TO {sql_literal(outputs["content_nodes"])}
  (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 122880)
""",
        )
    if not outputs["content_edges"].exists() or outputs["content_edges"].stat().st_size == 0:
        sql += marked(
            "prepare_content_edges",
            f"""
COPY (
    SELECT NULL::VARCHAR AS ":START_ID(SNBContent)",
           NULL::VARCHAR AS ":END_ID(SNBContent)",
           NULL::VARCHAR AS ":TYPE"
    LIMIT 0
) TO {sql_literal(outputs["content_edges"])} (FORMAT PARQUET)
""",
        )
    if not outputs["person_nodes"].exists() or outputs["person_nodes"].stat().st_size == 0:
        sql += marked(
            "prepare_person_nodes",
            f"""
COPY (
    SELECT 'person:' || id::VARCHAR AS "external_id:ID(SNBPerson)",
           id::BIGINT AS "person_id:long",
           firstName::VARCHAR AS "first_name:string",
           lastName::VARCHAR AS "last_name:string",
           epoch_ms(birthday)::DATE AS "birthday:date",
           'Person' AS ":LABEL"
    FROM read_csv({person}, delim='|', header=true, auto_detect=true)
) TO {sql_literal(outputs["person_nodes"])}
  (FORMAT PARQUET, COMPRESSION ZSTD)
""",
        )
    if not outputs["person_edges"].exists() or outputs["person_edges"].stat().st_size == 0:
        sql += marked(
            "prepare_person_edges",
            f"""
COPY (
    SELECT 'person:' || "Person.id"::VARCHAR AS ":START_ID(SNBPerson)",
           'person:' || "Person.id_1"::VARCHAR AS ":END_ID(SNBPerson)",
           'KNOWS' AS ":TYPE"
    FROM read_csv({knows}, delim='|', header=true, auto_detect=true)
    UNION ALL
    SELECT 'person:' || "Person.id_1"::VARCHAR AS ":START_ID(SNBPerson)",
           'person:' || "Person.id"::VARCHAR AS ":END_ID(SNBPerson)",
           'KNOWS' AS ":TYPE"
    FROM read_csv({knows}, delim='|', header=true, auto_detect=true)
) TO {sql_literal(outputs["person_edges"])}
  (FORMAT PARQUET, COMPRESSION ZSTD, ROW_GROUP_SIZE 122880)
""",
        )
    sql += ".timer off\n"
    completed = run_cli(cli, sql)
    if completed.returncode:
        raise RuntimeError(
            "SNB input preparation failed\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    for path in outputs.values():
        if not path.exists() or path.stat().st_size == 0:
            raise RuntimeError(f"preparation did not create {path}")
    return parse_timers(completed.stdout + "\n" + completed.stderr), outputs


def inspect_database(
    cli: Path, extension: Path, database: Path
) -> dict[str, tuple[int, int, int]]:
    if not database.exists():
        return {}
    sql = """
.mode csv
.headers off
SELECT g.graph_name, g.graph_id, count.vertex_count, count.edge_count
FROM gql_internal.graphs g
JOIN gql_graphs() count USING (graph_name)
WHERE g.graph_name IN ('snb_content', 'snb_person')
ORDER BY g.graph_name;
"""
    completed = run_cli(cli, sql, database, extension)
    if completed.returncode:
        return {}
    result: dict[str, tuple[int, int, int]] = {}
    for row in csv.reader(completed.stdout.splitlines()):
        if len(row) == 4:
            result[row[0]] = (int(row[1]), int(row[2]), int(row[3]))
    return result


def load_graphs(
    cli: Path,
    extension: Path,
    database: Path,
    inputs: dict[str, Path],
    threads: int,
    memory_limit: str,
) -> tuple[dict[str, float], dict[str, tuple[int, int, int]]]:
    expected = {
        "snb_content": (29_301_171, 0),
        "snb_person": (65_645, 3_877_032),
    }
    existing = inspect_database(cli, extension, database)
    if existing:
        for name, (vertices, edges) in expected.items():
            if name not in existing or existing[name][1:] != (vertices, edges):
                raise RuntimeError(
                    f"{database} contains an incomplete or unexpected {name} graph: "
                    f"{existing.get(name)}"
                )
        return {}, existing
    if database.exists():
        raise RuntimeError(
            f"{database} exists but does not contain both expected SNB graphs; "
            "choose a new --database path"
        )
    database.parent.mkdir(parents=True, exist_ok=True)
    sql = f"""
PRAGMA threads={threads};
SET memory_limit={sql_literal(memory_limit)};
.timer on
CREATE GRAPH snb_content ANY;
"""
    sql += marked(
        "copy_content_graph",
        f"""
COPY GRAPH snb_content FROM (
    VERTICES {sql_literal(inputs["content_nodes"])},
    EDGES {sql_literal(inputs["content_edges"])}
) FORMAT GRAPH OPTIONS (VALIDATE FALSE)
""",
    )
    sql += "CREATE GRAPH snb_person ANY;\n"
    sql += marked(
        "copy_person_graph",
        f"""
COPY GRAPH snb_person FROM (
    VERTICES {sql_literal(inputs["person_nodes"])},
    EDGES {sql_literal(inputs["person_edges"])}
) FORMAT GRAPH OPTIONS (VALIDATE FALSE)
""",
    )
    sql += marked("build_person_csr", "CALL gql_build_csr('snb_person')")
    sql += marked("checkpoint", "CHECKPOINT")
    sql += ".timer off\n"
    completed = run_cli(cli, sql, database, extension)
    if completed.returncode:
        raise RuntimeError(
            "SNB graph load failed\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    graphs = inspect_database(cli, extension, database)
    for name, (vertices, edges) in expected.items():
        if name not in graphs or graphs[name][1:] != (vertices, edges):
            raise RuntimeError(
                f"{name}: expected {(vertices, edges)}, found {graphs.get(name)}"
            )
    return parse_timers(completed.stdout + "\n" + completed.stderr), graphs


def parse_profiles(stderr: str, prefix: str) -> list[dict[str, Any]]:
    decoder = json.JSONDecoder()
    profiles: list[dict[str, Any]] = []
    offset = 0
    while True:
        start = stderr.find("{", offset)
        if start < 0:
            break
        try:
            value, end = decoder.raw_decode(stderr, start)
        except json.JSONDecodeError:
            offset = start + 1
            continue
        if value.get("query_name", "").lstrip().startswith(prefix):
            profiles.append(value)
        offset = end
    return profiles


def query_count(
    cli: Path,
    extension: Path,
    database: Path,
    query: str,
    graph: str | None,
    threads: int,
    memory_limit: str,
) -> int:
    sql = ".mode csv\n.headers off\n"
    if graph is not None:
        sql += (
            ".output /dev/null\n"
            f"SESSION SET GRAPH {graph};\n"
            ".output stdout\n"
        )
    sql += query.rstrip().rstrip(";") + ";\n"
    completed = run_cli(cli, sql, database, extension)
    if completed.returncode:
        raise RuntimeError(
            f"query validation failed\nstdout:\n{completed.stdout}"
            f"\nstderr:\n{completed.stderr}"
        )
    rows = list(csv.reader(completed.stdout.splitlines()))
    if len(rows) != 1 or len(rows[0]) != 1:
        raise RuntimeError(f"expected one scalar result, found {rows}")
    return int(rows[0][0])


def profile_query(
    cli: Path,
    extension: Path,
    database: Path,
    name: str,
    query: str,
    prefix: str,
    graph: str | None,
    expected_count: int,
    threads: int,
    memory_limit: str,
    warmups: int,
    runs: int,
) -> dict[str, Any]:
    actual = query_count(
        cli, extension, database, query, graph, threads, memory_limit
    )
    if actual != expected_count:
        raise RuntimeError(f"{name}: expected {expected_count}, found {actual}")
    command = [
        str(cli),
        "-unsigned",
        "-no-init",
        "-storage-version",
        "v1.5.0",
        str(database),
        "-cmd",
        f"LOAD {sql_literal(extension)};",
        "-cmd",
        f"PRAGMA threads={threads};",
        "-cmd",
        f"SET memory_limit={sql_literal(memory_limit)};",
    ]
    if graph is not None:
        command += ["-cmd", f"SESSION SET GRAPH {graph};"]
    payload = ".output /dev/null\nPRAGMA enable_profiling='json';\n"
    payload += (query.rstrip().rstrip(";") + ";\n") * (warmups + runs)
    completed = subprocess.run(
        command,
        input=payload,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(
            f"{name} failed\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    profiles = parse_profiles(completed.stderr, prefix)
    if len(profiles) != warmups + runs:
        raise RuntimeError(
            f"{name}: expected {warmups + runs} profiles, found {len(profiles)}"
        )
    measured = profiles[warmups:]
    return {
        "result_count": actual,
        "warmups": warmups,
        "runs": runs,
        "latency_seconds": summarize([entry["latency"] for entry in measured]),
        "cpu_seconds": summarize([entry["cpu_time"] for entry in measured]),
        "cumulative_rows_scanned": summarize(
            [float(entry["cumulative_rows_scanned"]) for entry in measured]
        ),
    }


def source_metadata() -> dict[str, Any]:
    root = Path(__file__).resolve().parents[2]
    commit = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=root,
        text=True,
        capture_output=True,
        check=True,
    ).stdout.strip()
    status = subprocess.run(
        ["git", "status", "--porcelain", "--untracked-files=no"],
        cwd=root,
        text=True,
        capture_output=True,
        check=True,
    ).stdout
    return {"commit": commit, "dirty": bool(status.strip())}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=Path("build/release/duckdb"))
    parser.add_argument(
        "--extension",
        type=Path,
        default=Path(
            "build/release/extension/duckgql/duckgql.duckdb_extension"
        ),
    )
    parser.add_argument(
        "--dataset",
        type=Path,
        default=Path(
            "build/benchmarks/snb10/extracted/"
            "social_network-sf10-CsvBasic-LongDateFormatter"
        ),
    )
    parser.add_argument(
        "--parameters",
        type=Path,
        default=Path(
            "build/benchmarks/snb10/extracted/substitution_parameters-sf10"
        ),
    )
    parser.add_argument(
        "--prepared-dir",
        type=Path,
        default=Path("build/benchmarks/snb10/prepared"),
    )
    parser.add_argument(
        "--database",
        type=Path,
        default=Path("build/benchmarks/snb10/snb10.duckdb"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("build/benchmarks/snb10/results.json"),
    )
    parser.add_argument("--threads", type=int, default=8)
    parser.add_argument("--memory-limit", default="20GB")
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--runs", type=int, default=5)
    args = parser.parse_args()

    if args.threads < 1 or args.warmups < 0 or args.runs < 1:
        raise SystemExit("threads/runs must be positive and warmups non-negative")
    cli = args.duckdb.resolve()
    extension = args.extension.resolve()
    dataset = args.dataset.resolve()
    parameters = args.parameters.resolve() if args.parameters.exists() else None
    prepared = args.prepared_dir.resolve()
    database = args.database.resolve()
    output = args.output.resolve()
    for path in (cli, extension, dataset):
        if not path.exists():
            raise SystemExit(f"missing required path: {path}")

    started = time.perf_counter()
    preparation, inputs = prepare_inputs(
        cli, dataset, prepared, args.threads, args.memory_limit
    )
    load, graphs = load_graphs(
        cli,
        extension,
        database,
        inputs,
        args.threads,
        args.memory_limit,
    )
    seed, seed_source = read_official_seed(parameters)
    content_id, content_vertices, _ = graphs["snb_content"]
    person_id, person_vertices, person_edges = graphs["snb_person"]
    content_table = f"gql_data.graph_{content_id}_vertices"
    person_table = f"gql_data.graph_{person_id}_vertices"
    knows_table = f"gql_data.graph_{person_id}_edges"

    workloads = [
        (
            "native_message_label_count",
            f"SELECT count(*) FROM {content_table} "
            "WHERE list_contains(__gql_label, 'message')",
            "SELECT count(*)",
            None,
            content_vertices,
        ),
        (
            "gql_message_label_count",
            "MATCH (message:Message) RETURN count(*)",
            "MATCH (message:Message)",
            "snb_content",
            content_vertices,
        ),
        (
            "native_post_label_count",
            f"SELECT count(*) FROM {content_table} "
            "WHERE list_contains(__gql_label, 'post')",
            "SELECT count(*)",
            None,
            7_435_696,
        ),
        (
            "gql_post_label_count",
            "MATCH (post:Post) RETURN count(*)",
            "MATCH (post:Post)",
            "snb_content",
            7_435_696,
        ),
        (
            "native_person_lookup",
            f"SELECT count(*) FROM {person_table} WHERE person_id = {seed}",
            "SELECT count(*)",
            None,
            1,
        ),
        (
            "gql_person_lookup",
            f"MATCH (person:Person) WHERE person.person_id = {seed} RETURN count(*)",
            "MATCH (person:Person)",
            "snb_person",
            1,
        ),
    ]

    native_one_hop = f"""
SELECT count(*)
FROM {person_table} source
JOIN {knows_table} edge ON edge.__gql_source_id = source.__gql_id
JOIN {person_table} target ON target.__gql_id = edge.__gql_target_id
WHERE source.person_id = {seed}
  AND list_contains(source.__gql_label, 'person')
  AND edge.__gql_type = 'knows'
  AND list_contains(target.__gql_label, 'person')
"""
    gql_one_hop = f"""
MATCH (source:Person)-[:KNOWS]->(target:Person)
WHERE source.person_id = {seed}
RETURN count(*)
"""
    one_hop_count = query_count(
        cli,
        extension,
        database,
        native_one_hop,
        None,
        args.threads,
        args.memory_limit,
    )
    workloads.extend(
        [
            (
                "native_one_hop_count",
                native_one_hop,
                "SELECT count(*)",
                None,
                one_hop_count,
            ),
            (
                "gql_one_hop_count",
                gql_one_hop,
                "MATCH (source:Person)",
                "snb_person",
                one_hop_count,
            ),
        ]
    )

    native_two_hop = f"""
SELECT count(*)
FROM {person_table} source
JOIN {knows_table} edge1 ON edge1.__gql_source_id = source.__gql_id
JOIN {person_table} middle ON middle.__gql_id = edge1.__gql_target_id
JOIN {knows_table} edge2 ON edge2.__gql_source_id = middle.__gql_id
JOIN {person_table} target ON target.__gql_id = edge2.__gql_target_id
WHERE source.person_id = {seed}
  AND list_contains(source.__gql_label, 'person')
  AND edge1.__gql_type = 'knows'
  AND list_contains(middle.__gql_label, 'person')
  AND edge2.__gql_type = 'knows'
  AND list_contains(target.__gql_label, 'person')
  AND edge1.__gql_edge_id <> edge2.__gql_edge_id
"""
    gql_two_hop = f"""
MATCH (source:Person)-[:KNOWS]->{{2}}(target:Person)
WHERE source.person_id = {seed}
RETURN count(*)
"""
    two_hop_count = query_count(
        cli,
        extension,
        database,
        native_two_hop,
        None,
        args.threads,
        args.memory_limit,
    )
    workloads.extend(
        [
            (
                "native_two_hop_count",
                native_two_hop,
                "SELECT count(*)",
                None,
                two_hop_count,
            ),
            (
                "gql_two_hop_count",
                gql_two_hop,
                "MATCH (source:Person)",
                "snb_person",
                two_hop_count,
            ),
        ]
    )

    query_results: dict[str, Any] = {}
    for name, query, prefix, graph, expected in workloads:
        print(f"[{name}] validating and profiling", flush=True)
        query_results[name] = profile_query(
            cli,
            extension,
            database,
            name,
            query,
            prefix,
            graph,
            expected,
            args.threads,
            args.memory_limit,
            args.warmups,
            args.runs,
        )
        median = query_results[name]["latency_seconds"]["median"]
        print(f"[{name}] median {median:.6f}s", flush=True)

    payload = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "benchmark_kind": "DuckGQL focused SNB projection; not an official LDBC result",
        "source": source_metadata(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "configuration": {
            "threads": args.threads,
            "memory_limit": args.memory_limit,
            "warmups": args.warmups,
            "runs": args.runs,
        },
        "dataset": {
            "name": dataset.name,
            "path": str(dataset),
            "parameters": str(parameters) if parameters else None,
            "seed_person_id": seed,
            "seed_source": seed_source,
            "projections": {
                "snb_content": {
                    "vertices": content_vertices,
                    "edges": 0,
                    "labels": ["Post;Message", "Comment;Message"],
                },
                "snb_person": {
                    "vertices": person_vertices,
                    "edges": person_edges,
                    "labels": ["Person"],
                    "edge_types": ["KNOWS"],
                },
            },
            "prepared_input_bytes": sum(
                path.stat().st_size for path in inputs.values()
            ),
        },
        "preparation_seconds": preparation,
        "load_seconds": load,
        "database_bytes": database.stat().st_size,
        "queries": query_results,
        "total_wall_seconds": time.perf_counter() - started,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(payload, indent=2) + "\n")
    print(json.dumps({"output": str(output), "queries": query_results}, indent=2))


if __name__ == "__main__":
    main()
