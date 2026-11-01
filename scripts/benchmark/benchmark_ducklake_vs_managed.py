#!/usr/bin/env python3
"""Compare DuckLake-referenced and managed DuckGQL graphs on one SF10 topology.

The benchmark projects Person vertices and KNOWS edges from the same LDBC SNB
SF10 relational database. It records source preparation and graph registration
separately from direct MATCH, cold full-CSR construction, and warm PageRank.
This is an engineering comparison, not an official LDBC benchmark result.
"""

from __future__ import annotations

import argparse
import csv
import json
import platform
import statistics
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from duckdb_profile import normalize_profile


GRAPH_NAME = "snb_person_knows"


def sql_literal(value: str | Path) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def run_cli(
    cli: Path,
    database: Path,
    sql: str,
    timeout: float,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(cli), "-unsigned", "-no-init", str(database)],
        input=sql,
        text=True,
        capture_output=True,
        check=False,
        timeout=timeout,
    )


def require_success(completed: subprocess.CompletedProcess[str], action: str) -> None:
    if completed.returncode:
        raise RuntimeError(
            f"{action} failed\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )


def scalar(
    cli: Path,
    database: Path,
    sql: str,
    timeout: float,
) -> str:
    completed = run_cli(
        cli,
        database,
        f".mode csv\n.headers off\n{sql.rstrip().rstrip(';')};\n",
        timeout,
    )
    require_success(completed, "scalar query")
    rows = list(csv.reader(completed.stdout.splitlines()))
    if len(rows) != 1 or len(rows[0]) != 1:
        raise RuntimeError(f"expected one scalar value, found: {completed.stdout!r}")
    return rows[0][0]


def parse_profiles(stderr: str) -> list[dict[str, Any]]:
    decoder = json.JSONDecoder()
    profiles: list[dict[str, Any]] = []
    offset = 0
    while True:
        start = stderr.find("{", offset)
        if start < 0:
            break
        try:
            value, end = decoder.raw_decode(stderr, start)
            value = normalize_profile(value)
        except json.JSONDecodeError:
            offset = start + 1
            continue
        if isinstance(value, dict) and "latency" in value and "query_name" in value:
            profiles.append(value)
        offset = end
    return profiles


def summarize(values: list[float]) -> dict[str, float]:
    return {
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
        "mean": statistics.mean(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def tree_bytes(path: Path) -> int:
    if not path.exists():
        return 0
    if path.is_file():
        return path.stat().st_size
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


def gql_prefix(extension: Path, threads: int, memory_limit: str) -> str:
    return f"""
LOAD {sql_literal(extension)};
PRAGMA threads={threads};
SET memory_limit={sql_literal(memory_limit)};
SET preserve_insertion_order=false;
"""


def referenced_prefix(
    extension: Path,
    catalog: Path,
    data_path: Path,
    threads: int,
    memory_limit: str,
) -> str:
    return (
        gql_prefix(extension, threads, memory_limit)
        + f"""
LOAD ducklake;
ATTACH {sql_literal('ducklake:' + str(catalog))} AS lake
    (DATA_PATH {sql_literal(data_path)});
"""
    )


def ensure_source_exports(
    args: argparse.Namespace,
    expected_vertices: int,
    expected_edges: int,
) -> dict[str, Any]:
    vertices = args.work_directory / "person-vertices.parquet"
    edges = args.work_directory / "knows-edges.parquet"
    existing_vertices = (
        int(
            scalar(
                args.duckdb,
                args.source_database,
                f"SELECT count(*) FROM read_parquet({sql_literal(vertices)})",
                args.timeout,
            )
        )
        if vertices.exists()
        else None
    )
    existing_edges = (
        int(
            scalar(
                args.duckdb,
                args.source_database,
                f"SELECT count(*) FROM read_parquet({sql_literal(edges)})",
                args.timeout,
            )
        )
        if edges.exists()
        else None
    )
    if (existing_vertices, existing_edges) == (expected_vertices, expected_edges):
        return {"reused": True, "seconds": 0.0, "vertices": vertices, "edges": edges}
    if vertices.exists() or edges.exists():
        raise RuntimeError(
            "existing managed export is incomplete or has unexpected counts; "
            "choose a new --work-directory"
        )

    sql = f"""
PRAGMA threads={args.threads};
SET memory_limit={sql_literal(args.memory_limit)};
SET preserve_insertion_order=false;
COPY (
    SELECT p_personid::BIGINT AS ":ID",
           'Person'::VARCHAR AS ":LABEL",
           p_personid::BIGINT AS id
    FROM person
) TO {sql_literal(vertices)} (FORMAT PARQUET, COMPRESSION ZSTD);
COPY (
    SELECT k_person1id::BIGINT AS ":START_ID",
           k_person2id::BIGINT AS ":END_ID",
           'KNOWS'::VARCHAR AS ":TYPE"
    FROM knows
) TO {sql_literal(edges)} (FORMAT PARQUET, COMPRESSION ZSTD);
"""
    started = time.perf_counter()
    completed = run_cli(args.duckdb, args.source_database, sql, args.timeout)
    require_success(completed, "managed source export")
    return {
        "reused": False,
        "seconds": time.perf_counter() - started,
        "vertices": vertices,
        "edges": edges,
    }


def ensure_managed_graph(
    args: argparse.Namespace,
    exports: dict[str, Any],
    expected_vertices: int,
    expected_edges: int,
) -> dict[str, Any]:
    if args.managed_database.exists():
        prefix = gql_prefix(args.extension, args.threads, args.memory_limit)
        counts = scalar(
            args.duckdb,
            args.managed_database,
            prefix
            + f"""
.once /dev/null
SESSION SET GRAPH {GRAPH_NAME};
MATCH (source:Person)-[:KNOWS]->(target:Person)
RETURN count(*)
""",
            args.timeout,
        )
        if counts != str(expected_edges):
            raise RuntimeError("existing managed graph has unexpected edge count")
        return {"reused": True, "seconds": 0.0}

    sql = gql_prefix(args.extension, args.threads, args.memory_limit) + f"""
CREATE GRAPH {GRAPH_NAME} ANY;
COPY GRAPH {GRAPH_NAME} FROM (
    VERTICES {sql_literal(exports['vertices'])},
    EDGES {sql_literal(exports['edges'])}
) FORMAT GRAPH OPTIONS (VALIDATE TRUE);
CHECKPOINT;
"""
    started = time.perf_counter()
    completed = run_cli(args.duckdb, args.managed_database, sql, args.timeout)
    require_success(completed, "managed COPY GRAPH")
    elapsed = time.perf_counter() - started
    return {"reused": False, "seconds": elapsed}


def ensure_ducklake_source(
    args: argparse.Namespace,
    expected_vertices: int,
    expected_edges: int,
) -> dict[str, Any]:
    if args.ducklake_catalog.exists():
        prefix = referenced_prefix(
            args.extension,
            args.ducklake_catalog,
            args.ducklake_data,
            args.threads,
            args.memory_limit,
        )
        counts = scalar(
            args.duckdb,
            Path(":memory:"),
            prefix
            + "SELECT (SELECT count(*) FROM lake.main.person_nodes) || ':' || "
            "(SELECT count(*) FROM lake.main.knows_edges)",
            args.timeout,
        )
        if counts != f"{expected_vertices}:{expected_edges}":
            raise RuntimeError("existing DuckLake projection has unexpected counts")
        return {"reused": True, "seconds": 0.0}

    sql = gql_prefix(args.extension, args.threads, args.memory_limit) + f"""
LOAD ducklake;
ATTACH {sql_literal(args.source_database)} AS raw (READ_ONLY);
ATTACH {sql_literal('ducklake:' + str(args.ducklake_catalog))} AS lake
    (DATA_PATH {sql_literal(args.ducklake_data)});
CREATE TABLE lake.main.person_nodes AS
SELECT p_personid::BIGINT AS person_key,
       p_personid::BIGINT AS id
FROM raw.main.person;
CREATE TABLE lake.main.knows_edges AS
SELECT k_person1id::BIGINT AS source_person_key,
       k_person2id::BIGINT AS target_person_key
FROM raw.main.knows;
CHECKPOINT;
"""
    started = time.perf_counter()
    completed = run_cli(args.duckdb, Path(":memory:"), sql, args.timeout)
    require_success(completed, "DuckLake source projection")
    return {"reused": False, "seconds": time.perf_counter() - started}


def ensure_referenced_graph(args: argparse.Namespace) -> dict[str, Any]:
    prefix = referenced_prefix(
        args.extension,
        args.ducklake_catalog,
        args.ducklake_data,
        args.threads,
        args.memory_limit,
    )
    if args.referenced_database.exists():
        schema_count = int(
            scalar(
                args.duckdb,
                args.referenced_database,
                prefix
                + "SELECT count(*) FROM duckdb_schemas() WHERE schema_name = 'gql_internal'",
                args.timeout,
            )
        )
        graph_count = (
            int(
                scalar(
                    args.duckdb,
                    args.referenced_database,
                    prefix
                    + f"SELECT count(*) FROM gql_internal.graphs WHERE graph_name = {sql_literal(GRAPH_NAME)}",
                    args.timeout,
                )
            )
            if schema_count
            else 0
        )
        if graph_count == 1:
            return {"reused": True, "seconds": 0.0}
        if graph_count != 0:
            raise RuntimeError("referenced database has an unexpected graph catalog")

    sql = prefix + f"""
CREATE GRAPH {GRAPH_NAME} TYPED {{
    (Person :Person {{id INT64 NOT NULL}}),
    (Person)-[:KNOWS]->(Person)
}}
FROM TABLES (
    VERTEX TABLE lake.main.person_nodes
        MAP TO NODE TYPE Person
        KEY (person_key)
        PROPERTIES (id AS id),
    EDGE TABLE lake.main.knows_edges
        MAP TO EDGE TYPE KNOWS
        SOURCE (source_person_key) REFERENCES NODE TYPE Person
        DESTINATION (target_person_key) REFERENCES NODE TYPE Person
)
OPTIONS (
    SNAPSHOT_POLICY 'LIVE',
    ACCESS_MODE 'READ_ONLY',
    VALIDATE TRUE
);
CHECKPOINT;
"""
    started = time.perf_counter()
    completed = run_cli(args.duckdb, args.referenced_database, sql, args.timeout)
    require_success(completed, "referenced CREATE GRAPH")
    return {"reused": False, "seconds": time.perf_counter() - started}


def mode_prefix(args: argparse.Namespace, mode: str) -> tuple[Path, str]:
    if mode == "managed":
        return (
            args.managed_database,
            gql_prefix(args.extension, args.threads, args.memory_limit),
        )
    return (
        args.referenced_database,
        referenced_prefix(
            args.extension,
            args.ducklake_catalog,
            args.ducklake_data,
            args.threads,
            args.memory_limit,
        ),
    )


def validate_mode(
    args: argparse.Namespace,
    mode: str,
    expected_vertices: int,
    expected_edges: int,
) -> dict[str, Any]:
    database, prefix = mode_prefix(args, mode)
    edge_count = int(
        scalar(
            args.duckdb,
            database,
            prefix
            + f"""
.once /dev/null
SESSION SET GRAPH {GRAPH_NAME};
MATCH (:Person)-[:KNOWS]->(:Person) RETURN count(*)
""",
            args.timeout,
        )
    )
    vertex_count = int(
        scalar(
            args.duckdb,
            database,
            prefix
            + f"""
.once /dev/null
SESSION SET GRAPH {GRAPH_NAME};
MATCH (:Person) RETURN count(*)
""",
            args.timeout,
        )
    )
    if (vertex_count, edge_count) != (expected_vertices, expected_edges):
        raise RuntimeError(
            f"{mode} graph has {(vertex_count, edge_count)}, expected "
            f"{(expected_vertices, expected_edges)}"
        )
    pagerank_result = scalar(
        args.duckdb,
        database,
        prefix
        + f"""
.once /dev/null
SESSION SET GRAPH {GRAPH_NAME};
.once /dev/null
CALL gql_build_csr({sql_literal(GRAPH_NAME)});
SELECT count(*)::VARCHAR || ':' || round(sum(rank), 12)::VARCHAR
FROM system.algo.pagerank({sql_literal(GRAPH_NAME)},
    vertex_label := 'Person', edge_label := 'KNOWS')
""",
        args.timeout,
    )
    pagerank_count, pagerank_sum = pagerank_result.split(":")
    if int(pagerank_count) != expected_vertices or abs(float(pagerank_sum) - 1.0) > 1e-9:
        raise RuntimeError(f"{mode} PageRank validation failed: {pagerank_result}")
    return {
        "vertices": vertex_count,
        "edges": edge_count,
        "pagerank_rows": int(pagerank_count),
        "pagerank_sum": float(pagerank_sum),
    }


def profile_repeated(
    args: argparse.Namespace,
    mode: str,
    statement: str,
    warmups: int,
    runs: int,
    build_csr: bool,
) -> dict[str, Any]:
    database, prefix = mode_prefix(args, mode)
    preparation = (
        f".once /dev/null\nCALL gql_build_csr({sql_literal(GRAPH_NAME)});\n"
        if build_csr
        else ""
    )
    statements = "\n".join(statement.rstrip().rstrip(";") + ";" for _ in range(warmups + runs))
    sql = prefix + f"""
SESSION SET GRAPH {GRAPH_NAME};
{preparation}
.output /dev/null
PRAGMA enable_profiling='json';
{statements}
"""
    completed = run_cli(args.duckdb, database, sql, args.timeout)
    require_success(completed, f"{mode} repeated profile")
    profiles = parse_profiles(completed.stderr)
    if len(profiles) != warmups + runs:
        raise RuntimeError(
            f"{mode}: expected {warmups + runs} profiles, found {len(profiles)}"
        )
    measured = profiles[warmups:]
    latency_samples = [float(item["latency"]) for item in measured]
    cpu_samples = [float(item["cpu_time"]) for item in measured]
    rows_scanned_samples = [
        float(item["cumulative_rows_scanned"]) for item in measured
    ]
    return {
        "warmups": warmups,
        "runs": runs,
        "latency_seconds": summarize(latency_samples),
        "latency_seconds_samples": latency_samples,
        "cpu_seconds": summarize(cpu_samples),
        "cpu_seconds_samples": cpu_samples,
        "rows_scanned": summarize(rows_scanned_samples),
    }


def profile_cold_csr(args: argparse.Namespace, mode: str) -> dict[str, Any]:
    latencies: list[float] = []
    cpu_times: list[float] = []
    rows_scanned: list[float] = []
    for _ in range(args.cold_runs):
        database, prefix = mode_prefix(args, mode)
        sql = prefix + f"""
.output /dev/null
PRAGMA enable_profiling='json';
CALL gql_build_csr({sql_literal(GRAPH_NAME)});
"""
        completed = run_cli(args.duckdb, database, sql, args.timeout)
        require_success(completed, f"{mode} cold CSR")
        profiles = parse_profiles(completed.stderr)
        if len(profiles) != 1:
            raise RuntimeError(f"{mode}: expected one CSR profile, found {len(profiles)}")
        profile = profiles[0]
        latencies.append(float(profile["latency"]))
        cpu_times.append(float(profile["cpu_time"]))
        rows_scanned.append(float(profile["cumulative_rows_scanned"]))
    return {
        "runs": args.cold_runs,
        "latency_seconds": summarize(latencies),
        "latency_seconds_samples": latencies,
        "cpu_seconds": summarize(cpu_times),
        "cpu_seconds_samples": cpu_times,
        "rows_scanned": summarize(rows_scanned),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, default=Path("build/release/duckdb"))
    parser.add_argument(
        "--extension",
        type=Path,
        default=Path("build/release/extension/duckgql/duckgql.duckdb_extension"),
    )
    parser.add_argument(
        "--source-database",
        type=Path,
        default=Path("build/benchmarks/snb10/snb10-relational.duckdb"),
    )
    parser.add_argument(
        "--work-directory",
        type=Path,
        default=Path("build/benchmarks/snb10/ducklake-vs-managed"),
    )
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--memory-limit", default="8GB")
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--cold-runs", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    args.duckdb = args.duckdb.resolve()
    args.extension = args.extension.resolve()
    args.source_database = args.source_database.resolve()
    args.work_directory = args.work_directory.resolve()
    args.managed_database = args.work_directory / "managed.duckdb"
    args.referenced_database = args.work_directory / "referenced.duckdb"
    args.ducklake_catalog = args.work_directory / "snb-person-knows.ducklake"
    args.ducklake_data = args.work_directory / "ducklake-data"
    args.output = (
        args.output.resolve()
        if args.output
        else args.work_directory / "results.json"
    )
    return args


def main() -> None:
    args = parse_args()
    for required in (args.duckdb, args.extension, args.source_database):
        if not required.exists():
            raise FileNotFoundError(required)
    args.work_directory.mkdir(parents=True, exist_ok=True)

    source_counts = scalar(
        args.duckdb,
        args.source_database,
        "SELECT (SELECT count(*) FROM person) || ':' || (SELECT count(*) FROM knows)",
        args.timeout,
    )
    expected_vertices, expected_edges = (int(value) for value in source_counts.split(":"))
    print(f"[source] {expected_vertices:,} Person vertices, {expected_edges:,} KNOWS edges", flush=True)

    print("[prepare] managed graph", flush=True)
    exports = ensure_source_exports(args, expected_vertices, expected_edges)
    managed_setup = ensure_managed_graph(
        args, exports, expected_vertices, expected_edges
    )
    print("[prepare] DuckLake graph", flush=True)
    ducklake_setup = ensure_ducklake_source(args, expected_vertices, expected_edges)
    referenced_setup = ensure_referenced_graph(args)

    validations: dict[str, Any] = {}
    for mode in ("managed", "referenced"):
        print(f"[validate] {mode}", flush=True)
        validations[mode] = validate_mode(
            args, mode, expected_vertices, expected_edges
        )

    match_query = "MATCH (:Person)-[:KNOWS]->(:Person) RETURN count(*)"
    pagerank_query = (
        "SELECT count(*), round(sum(rank), 9) "
        f"FROM system.algo.pagerank({sql_literal(GRAPH_NAME)}, "
        "vertex_label := 'Person', edge_label := 'KNOWS')"
    )
    measurements: dict[str, Any] = {"managed": {}, "referenced": {}}
    for mode in ("managed", "referenced"):
        print(f"[measure] {mode} direct MATCH", flush=True)
        measurements[mode]["direct_match"] = profile_repeated(
            args, mode, match_query, args.warmups, args.runs, False
        )

    for mode in ("managed", "referenced"):
        print(f"[measure] {mode} cold full CSR", flush=True)
        measurements[mode]["cold_full_csr"] = profile_cold_csr(args, mode)

    for mode in ("managed", "referenced"):
        print(f"[measure] {mode} warm PageRank", flush=True)
        measurements[mode]["warm_pagerank"] = profile_repeated(
            args, mode, pagerank_query, args.warmups, args.runs, True
        )

    result = {
        "kind": "ducklake-vs-managed-engineering-comparison",
        "created_at": datetime.now(timezone.utc).isoformat(),
        "graph": GRAPH_NAME,
        "source": {
            "database": str(args.source_database),
            "vertices": expected_vertices,
            "edges": expected_edges,
            "vertex_label": "Person",
            "edge_label": "KNOWS",
        },
        "configuration": {
            "duckdb": str(args.duckdb),
            "extension": str(args.extension),
            "threads": args.threads,
            "memory_limit": args.memory_limit,
            "warmups": args.warmups,
            "runs": args.runs,
            "cold_runs": args.cold_runs,
            "platform": platform.platform(),
        },
        "setup": {
            "managed_source_export": {
                "reused": exports["reused"],
                "seconds": exports["seconds"],
            },
            "managed_graph": managed_setup,
            "ducklake_source": ducklake_setup,
            "referenced_graph": referenced_setup,
        },
        "validation": validations,
        "measurements": measurements,
        "storage_bytes": {
            "managed_database": tree_bytes(args.managed_database),
            "managed_source_exports": tree_bytes(exports["vertices"])
            + tree_bytes(exports["edges"]),
            "ducklake_catalog": tree_bytes(args.ducklake_catalog),
            "ducklake_data": tree_bytes(args.ducklake_data),
            "referenced_graph_database": tree_bytes(args.referenced_database),
        },
        "limitations": [
            "Engineering comparison, not an official LDBC benchmark result.",
            "Both modes use the same SF10 Person/KNOWS projection and release binary.",
            "OS file caches are not dropped between runs.",
            "Setup phases are reported separately and are not included in query latency.",
        ],
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"[done] {args.output}", flush=True)


if __name__ == "__main__":
    main()
