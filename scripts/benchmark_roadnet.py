#!/usr/bin/env python3
"""Benchmark native DuckDB, COPY GRAPH GQL, and prepared CSR on roadNet-CA."""

from __future__ import annotations

import argparse
import json
import platform
import random
import re
import statistics
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


PHASE_MARKER = "__GQL_BENCH_PHASE__"
RESULT_MARKER = "__GQL_BENCH_RESULT__"
BUILD_MARKER = "__GQL_CSR_BUILD__"
TIMER_RE = re.compile(r"Run Time \(s\): real ([0-9.]+)")
RSS_RE = re.compile(r"^\s*(\d+)\s+maximum resident set size\s*$", re.MULTILINE)
GNU_RSS_RE = re.compile(r"Maximum resident set size \(kbytes\):\s*(\d+)")


def sql_literal(value: Path | str) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def marked_statement(name: str, statement: str) -> str:
    return f".print {PHASE_MARKER}{name}\n{statement.rstrip(';')};\n"


def prepare_graph_inputs(
    cli: Path, dataset: Path, directory: Path, threads: int
) -> tuple[Path, Path]:
    nodes = directory / "nodes.csv"
    edges = directory / "edges.csv"
    sql = f"""
PRAGMA threads={threads};
CREATE TEMP TABLE road_edges_raw AS
SELECT source_id, target_id
FROM read_csv(
    {sql_literal(dataset)}, delim='\\t', header=false, skip=4,
    columns={{'source_id':'BIGINT','target_id':'BIGINT'}}
);
COPY (
    SELECT id::VARCHAR AS "external_id:ID(RoadNet)",
           id AS "native_id:long",
           'intersection' AS ":LABEL"
    FROM (
        SELECT source_id AS id FROM road_edges_raw
        UNION
        SELECT target_id AS id FROM road_edges_raw
    ) vertices
) TO {sql_literal(nodes)} (FORMAT CSV, HEADER);
COPY (
    SELECT source_id::VARCHAR AS ":START_ID(RoadNet)",
           target_id::VARCHAR AS ":END_ID(RoadNet)",
           'road' AS ":TYPE"
    FROM road_edges_raw
) TO {sql_literal(edges)} (FORMAT CSV, HEADER);
"""
    completed = subprocess.run(
        [str(cli), "-unsigned", "-no-init"],
        input=sql,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(f"input preparation failed: {completed.stderr}")
    return nodes, edges


def table_load_sql(dataset: Path) -> tuple[str, str, str]:
    source = f"""
CREATE TEMP TABLE road_edges_raw AS
SELECT source_id, target_id
FROM read_csv(
    {sql_literal(dataset)},
    delim = '\\t', header = false, skip = 4,
    columns = {{'source_id': 'BIGINT', 'target_id': 'BIGINT'}}
)
"""
    vertices = """
CREATE TABLE intersections AS
SELECT id, 'intersection'::VARCHAR AS label, id AS external_id
FROM (
    SELECT source_id AS id FROM road_edges_raw
    UNION
    SELECT target_id AS id FROM road_edges_raw
) ids
"""
    edges = """
CREATE TABLE roads AS
SELECT row_number() OVER ()::BIGINT AS edge_id,
       source_id, target_id, 'road'::VARCHAR AS edge_type
FROM road_edges_raw
"""
    return source, vertices, edges


def setup_sql(
    case: str,
    dataset: Path,
    threads: int,
    timed: bool,
    nodes: Path | None = None,
    edges: Path | None = None,
) -> str:
    source_sql, vertex_table_sql, edge_table_sql = table_load_sql(dataset)
    sql = f"PRAGMA threads={threads};\n"
    if case == "native_tables":
        if timed:
            sql += ".timer on\n"
            sql += marked_statement("source_load", source_sql)
            sql += marked_statement("vertex_load", vertex_table_sql)
            sql += marked_statement("edge_load", edge_table_sql)
        else:
            sql += source_sql + ";\n" + vertex_table_sql + ";\n" + edge_table_sql + ";\n"
    else:
        if nodes is None or edges is None:
            raise ValueError("COPY GRAPH cases require prepared node and edge files")
        validate = "TRUE" if case == "copy_graph_validated" else "FALSE"
        copy_graph = f"""
COPY GRAPH roadnet FROM (
    VERTICES {sql_literal(nodes)},
    EDGES {sql_literal(edges)}
) FORMAT NEO4J OPTIONS (VALIDATE {validate})
"""
        if timed:
            sql += ".timer on\n"
            sql += marked_statement("graph_create", "CREATE GRAPH roadnet ANY")
            sql += marked_statement("copy_graph", copy_graph)
        else:
            sql += "CREATE GRAPH roadnet ANY;\n" + copy_graph + ";\n"
        sql += """
CREATE VIEW intersections AS
SELECT __gql_id AS id, __gql_label AS label, native_id AS external_id
FROM gql_data.graph_1_vertices;
CREATE VIEW roads AS
SELECT __gql_edge_id AS edge_id,
       __gql_source_id AS source_id,
       __gql_target_id AS target_id,
       __gql_type AS edge_type
FROM gql_data.graph_1_edges;
"""

    if timed:
        sql += marked_statement("checkpoint", "CHECKPOINT")
        sql += ".timer off\n"
    else:
        sql += "CHECKPOINT;\n"
    sql += (
        f"SELECT '{RESULT_MARKER}', "
        "(SELECT count(*) FROM intersections), "
        "(SELECT count(*) FROM roads);\n"
    )
    return sql


def parse_phases(output: str) -> dict[str, float]:
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
        raise RuntimeError(f"missing timer output for phase {pending}")
    return phases


def parse_counts(output: str) -> tuple[int, int]:
    for line in output.splitlines():
        if line.startswith(RESULT_MARKER):
            fields = line.split(",")
            if len(fields) == 3:
                return int(fields[1]), int(fields[2])
    raise RuntimeError("benchmark result counts were not found")


def peak_rss_bytes(stderr: str) -> int | None:
    match = RSS_RE.search(stderr)
    if match:
        return int(match.group(1))
    match = GNU_RSS_RE.search(stderr)
    return int(match.group(1)) * 1024 if match else None


def timed_command(cli: Path, database: Path) -> list[str]:
    flag = "-l" if platform.system() == "Darwin" else "-v"
    return [
        "/usr/bin/time",
        flag,
        str(cli),
        "-unsigned",
        "-no-init",
        "-storage-version",
        "v1.5.0",
        "-csv",
        "-noheader",
        str(database),
    ]


def run_load_trial(
    case: str,
    cli: Path,
    dataset: Path,
    threads: int,
    trial: int,
    warmup: bool,
) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix=f"duckdb-gql-roadnet-{case}-") as temp:
        temp_path = Path(temp)
        database = temp_path / "roadnet.duckdb"
        nodes: Path | None = None
        edges: Path | None = None
        if case != "native_tables":
            nodes, edges = prepare_graph_inputs(cli, dataset, temp_path, threads)
        started = time.perf_counter()
        completed = subprocess.run(
            timed_command(cli, database),
            input=setup_sql(case, dataset, threads, True, nodes, edges),
            text=True,
            capture_output=True,
            check=False,
        )
        wall = time.perf_counter() - started
        if completed.returncode:
            raise RuntimeError(
                f"{case} failed\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
            )
        phases = parse_phases(completed.stdout)
        counts = parse_counts(completed.stdout)
        size = database.stat().st_size
        wal = Path(str(database) + ".wal")
        if wal.exists():
            size += wal.stat().st_size
        return {
            "case": case,
            "trial": trial,
            "warmup": warmup,
            "phases_seconds": phases,
            "load_seconds": sum(v for k, v in phases.items() if k != "checkpoint"),
            "checkpoint_seconds": phases["checkpoint"],
            "process_wall_seconds": wall,
            "database_bytes": size,
            "peak_rss_bytes": peak_rss_bytes(completed.stderr),
            "vertex_rows": counts[0],
            "edge_rows": counts[1],
        }


def summarize(values: list[float]) -> dict[str, float]:
    return {
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
        "mean": statistics.mean(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def summarize_loads(trials: list[dict[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for case in sorted({entry["case"] for entry in trials}):
        rows = [entry for entry in trials if entry["case"] == case and not entry["warmup"]]
        result[case] = {
            "runs": len(rows),
            "load_seconds": summarize([entry["load_seconds"] for entry in rows]),
            "checkpoint_seconds": summarize(
                [entry["checkpoint_seconds"] for entry in rows]
            ),
            "process_wall_seconds": summarize(
                [entry["process_wall_seconds"] for entry in rows]
            ),
            "database_bytes": summarize([entry["database_bytes"] for entry in rows]),
            "peak_rss_bytes": summarize(
                [entry["peak_rss_bytes"] for entry in rows if entry["peak_rss_bytes"]]
            ),
            "phases_seconds": {
                phase: summarize(
                    [entry["phases_seconds"][phase] for entry in rows]
                )
                for phase in rows[0]["phases_seconds"]
            },
        }
    native = result["native_tables"]["load_seconds"]["median"]
    graph_rows = trials[0]["vertex_rows"] + trials[0]["edge_rows"]
    for entry in result.values():
        median = entry["load_seconds"]["median"]
        entry["graph_rows_per_second"] = graph_rows / median
        entry["load_slowdown_vs_native"] = median / native
    return result


def native_fixed_query(seed: int) -> str:
    branches = []
    for depth in range(1, 4):
        joins = ["intersections s"]
        previous = "s"
        edge_aliases = []
        filters = [f"s.external_id = {seed}", "s.label = 'intersection'"]
        for index in range(1, depth + 1):
            edge = f"e{index}"
            vertex = "t" if index == depth else f"m{index}"
            joins.append(f"JOIN roads {edge} ON {edge}.source_id = {previous}.id")
            joins.append(f"JOIN intersections {vertex} ON {vertex}.id = {edge}.target_id")
            edge_aliases.append(edge)
            filters.append(f"{edge}.edge_type = 'road'")
            filters.append(f"{vertex}.label = 'intersection'")
            previous = vertex
        for left in range(len(edge_aliases)):
            for right in range(left + 1, len(edge_aliases)):
                filters.append(
                    f"{edge_aliases[left]}.edge_id <> {edge_aliases[right]}.edge_id"
                )
        branches.append(
            f"SELECT {depth} AS hop, count(*) AS paths FROM "
            + " ".join(joins)
            + " WHERE "
            + " AND ".join(filters)
        )
    return (
        "SELECT SUM(paths)::BIGINT AS paths FROM ("
        + " UNION ALL ".join(branches)
        + ") fixed_lengths;"
    )


def native_recursive_query(seed: int) -> str:
    return f"""
WITH RECURSIVE paths(start_id, end_id, depth, used_edges) AS (
    SELECT source.id, source.id, 0::UBIGINT, []::BIGINT[]
    FROM intersections source
    WHERE source.external_id = {seed} AND source.label = 'intersection'
    UNION ALL
    SELECT paths.start_id, edge.target_id, paths.depth + 1,
           list_append(paths.used_edges, edge.edge_id)
    FROM paths
    JOIN roads edge ON edge.source_id = paths.end_id
    WHERE edge.edge_type = 'road'
      AND NOT list_contains(paths.used_edges, edge.edge_id)
)
SELECT target.external_id
FROM paths
JOIN intersections target ON target.id = paths.end_id
WHERE target.label = 'intersection'
LIMIT 100;
"""


def gql_fixed_query(seed: int) -> str:
    return f"""
MATCH (source:intersection)-[:road]->{{1,3}}(target:intersection)
WHERE source.native_id = {seed}
RETURN COUNT(*);
"""


def gql_csr_query(seed: int) -> str:
    return f"""
MATCH /*+ CSR */ (source:intersection)-[:road]->*(target:intersection)
WHERE source.native_id = {seed}
RETURN target.native_id
LIMIT 100;
"""


def parse_profiles(stderr: str, query_prefix: str) -> list[dict[str, Any]]:
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
        if value.get("query_name", "").lstrip().startswith(query_prefix):
            profiles.append(value)
        offset = end
    return profiles


def parse_build_seconds(stdout: str) -> float | None:
    after_marker = False
    for line in stdout.splitlines():
        if line.startswith(BUILD_MARKER):
            after_marker = True
            continue
        if after_marker:
            match = TIMER_RE.search(line)
            if match:
                return float(match.group(1))
    return None


def parse_build_snapshot(stdout: str) -> dict[str, int] | None:
    after_marker = False
    for line in stdout.splitlines():
        if line.startswith(BUILD_MARKER):
            after_marker = True
            continue
        if not after_marker:
            continue
        fields = line.split(",")
        if len(fields) == 6 and fields[0] == "roadnet":
            return {
                "graph_version": int(fields[1]),
                "vertex_count": int(fields[2]),
                "edge_count": int(fields[3]),
                "memory_bytes": int(fields[4]),
                "build_count": int(fields[5]),
            }
    return None


def profile_workload(
    name: str,
    cli: Path,
    extension: Path,
    database: Path,
    threads: int,
    query: str,
    query_prefix: str,
    warmups: int,
    runs: int,
    select_graph: bool,
    build_csr: bool,
    result_paths: int,
) -> dict[str, Any]:
    command = timed_command(cli, database)
    command += ["-cmd", f"LOAD {sql_literal(extension)};"]
    command += ["-cmd", f"PRAGMA threads={threads};"]
    if select_graph:
        command += ["-cmd", "SESSION SET GRAPH roadnet;"]
    payload = ""
    if build_csr:
        payload += f".print {BUILD_MARKER}\n.timer on\n"
        payload += "SELECT * FROM gql_build_csr('roadnet');\n.timer off\n"
    payload += ".output /dev/null\nPRAGMA enable_profiling='json';\n"
    payload += (query.rstrip() + "\n") * (warmups + runs)
    completed = subprocess.run(
        command, input=payload, text=True, capture_output=True, check=False
    )
    if completed.returncode:
        raise RuntimeError(
            f"{name} failed\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    profiles = parse_profiles(completed.stderr, query_prefix)
    expected = warmups + runs
    if len(profiles) != expected:
        raise RuntimeError(f"{name}: expected {expected} profiles, got {len(profiles)}")
    measured = profiles[warmups:]
    latencies = [entry["latency"] for entry in measured]
    cpu_times = [entry["cpu_time"] for entry in measured]
    median = statistics.median(latencies)
    return {
        "runs": runs,
        "warmups": warmups,
        "latency_seconds": summarize(latencies),
        "cpu_seconds": summarize(cpu_times),
        "result_paths": result_paths,
        "paths_per_second": result_paths / median,
        "csr_build_seconds": parse_build_seconds(completed.stdout),
        "csr_snapshot": parse_build_snapshot(completed.stdout),
        "process_peak_rss_bytes": peak_rss_bytes(completed.stderr),
    }


def run_query_benchmarks(
    cli: Path,
    extension: Path,
    dataset: Path,
    threads: int,
    seed: int,
    warmups: int,
    runs: int,
) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="duckdb-gql-roadnet-query-") as temp:
        temp_path = Path(temp)
        database = temp_path / "roadnet.duckdb"
        nodes, edges = prepare_graph_inputs(cli, dataset, temp_path, threads)
        setup = subprocess.run(
            [
                str(cli),
                "-unsigned",
                "-no-init",
                "-storage-version",
                "v1.5.0",
                str(database),
            ],
            input=setup_sql(
                "copy_graph_validated", dataset, threads, False, nodes, edges
            ),
            text=True,
            capture_output=True,
            check=False,
        )
        if setup.returncode:
            raise RuntimeError(f"query database setup failed: {setup.stderr}")

        workloads = [
            (
                "native_fixed_1_3",
                native_fixed_query(seed),
                "SELECT SUM(paths)",
                False,
                False,
                312,
            ),
            (
                "table_gql_fixed_1_3",
                gql_fixed_query(seed),
                "MATCH (source:intersection)",
                True,
                False,
                312,
            ),
            (
                "native_recursive_first_100",
                native_recursive_query(seed),
                "WITH RECURSIVE paths",
                False,
                False,
                100,
            ),
            (
                "prepared_csr_first_100",
                gql_csr_query(seed),
                "MATCH /*+ CSR */",
                True,
                True,
                100,
            ),
        ]
        result: dict[str, Any] = {}
        for name, query, prefix, graph, build, paths in workloads:
            result[name] = profile_workload(
                name,
                cli,
                extension,
                database,
                threads,
                query,
                prefix,
                warmups,
                runs,
                graph,
                build,
                paths,
            )
        return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=Path("build/release/duckdb"))
    parser.add_argument(
        "--extension",
        type=Path,
        default=Path("build/release/extension/gql/gql.duckdb_extension"),
    )
    parser.add_argument(
        "--dataset",
        type=Path,
        default=Path("data/roadnet-ca/roadNet-CA.txt.gz"),
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--threads", type=int, default=15)
    parser.add_argument("--seed", type=int, default=562818)
    parser.add_argument(
        "--query-only",
        action="store_true",
        help="reuse the load results already present in --output",
    )
    args = parser.parse_args()

    cli = args.duckdb.resolve()
    extension = args.extension.resolve()
    dataset = args.dataset.resolve()
    for path in (cli, extension, dataset):
        if not path.exists():
            raise SystemExit(f"missing required path: {path}")
    if args.runs < 1 or args.warmups < 0:
        raise SystemExit("runs must be positive and warmups non-negative")

    previous: dict[str, Any] = {}
    if args.query_only:
        if not args.output.exists():
            raise SystemExit("--query-only requires an existing --output file")
        previous = json.loads(args.output.read_text())

    load_trials: list[dict[str, Any]] = previous.get("load_trials", [])
    load_summary: dict[str, Any] = previous.get("load_summary", {})
    if not args.query_only:
        cases = ["native_tables", "copy_graph_fast", "copy_graph_validated"]
        randomizer = random.Random(20260719)
        for trial in range(args.warmups + args.runs):
            order = cases.copy()
            randomizer.shuffle(order)
            for case in order:
                load_trials.append(
                    run_load_trial(
                        case,
                        cli,
                        dataset,
                        args.threads,
                        trial,
                        trial < args.warmups,
                    )
                )
        load_summary = summarize_loads(load_trials)

    payload: dict[str, Any] = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "duckdb": str(cli),
        "extension": str(extension),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "threads": args.threads,
        "runs": args.runs,
        "warmups": args.warmups,
        "dataset": {
            "name": "roadNet-CA",
            "path": str(dataset),
            "compressed_bytes": dataset.stat().st_size,
            "vertex_rows": 1965206,
            "edge_rows": 5533214,
            "seed_vertex": args.seed,
        },
        "load_trials": load_trials,
        "load_summary": load_summary,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n")
    try:
        payload["query_summary"] = run_query_benchmarks(
            cli,
            extension,
            dataset,
            args.threads,
            args.seed,
            args.warmups,
            args.runs,
        )
    except Exception as error:
        payload["query_error"] = str(error)
        args.output.write_text(json.dumps(payload, indent=2) + "\n")
        raise
    args.output.write_text(json.dumps(payload, indent=2) + "\n")
    print(
        json.dumps(
            {
                "output": str(args.output),
                "summary": payload["load_summary"],
                "queries": payload["query_summary"],
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
