#!/usr/bin/env python3
"""Benchmark COPY GRAPH against an equivalent native DuckDB SNAP load."""

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


PHASE_MARKER = "__GQL_COPY_BENCH_PHASE__"
RESULT_MARKER = "__GQL_COPY_BENCH_RESULT__"
TIMER_RE = re.compile(r"Run Time \(s\): real ([0-9.]+)")
RSS_RE = re.compile(r"^\s*(\d+)\s+maximum resident set size\s*$", re.MULTILINE)
GNU_RSS_RE = re.compile(r"Maximum resident set size \(kbytes\):\s*(\d+)")


def sql_literal(value: Path | str) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def marked_statement(name: str, statement: str) -> str:
    return f".print {PHASE_MARKER}{name}\n{statement.rstrip(';')};\n"


def prepare_inputs(
    cli: Path,
    source: Path,
    directory: Path,
    threads: int,
    skip_rows: int,
    vertex_label: str,
    edge_type: str,
) -> tuple[Path, Path]:
    nodes = directory / "nodes.csv"
    edges = directory / "edges.csv"
    if nodes.exists() and edges.exists():
        return nodes, edges
    directory.mkdir(parents=True, exist_ok=True)
    sql = f"""
PRAGMA threads={threads};
CREATE TEMP TABLE raw AS
SELECT source_id, target_id
FROM read_csv(
    {sql_literal(source)}, delim='\\t', header=false, skip={skip_rows},
    columns={{'source_id':'BIGINT','target_id':'BIGINT'}}
);
COPY (
    SELECT id::VARCHAR AS "external_id:ID(RoadNet)",
           {sql_literal(vertex_label)} AS ":LABEL"
    FROM (
        SELECT source_id AS id FROM raw
        UNION
        SELECT target_id AS id FROM raw
    ) vertices
) TO {sql_literal(nodes)} (FORMAT CSV, HEADER);
COPY (
    SELECT source_id::VARCHAR AS ":START_ID(RoadNet)",
           target_id::VARCHAR AS ":END_ID(RoadNet)",
           {sql_literal(edge_type)} AS ":TYPE"
    FROM raw
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
        raise RuntimeError(f"SNAP input preparation failed: {completed.stderr}")
    return nodes, edges


def native_load_statements(nodes: Path, edges: Path) -> list[tuple[str, str]]:
    return [
        (
            "node_stage",
            f"""CREATE TEMP TABLE raw_nodes AS
SELECT row_number() OVER ()::UBIGINT AS row_id, *
FROM read_csv({sql_literal(nodes)}, header=true, auto_detect=true, all_varchar=true)""",
        ),
        (
            "edge_stage",
            f"""CREATE TEMP TABLE raw_edges AS
SELECT row_number() OVER ()::UBIGINT AS row_id, *
FROM read_csv({sql_literal(edges)}, header=true, auto_detect=true, all_varchar=true)""",
        ),
        (
            "vertex_load",
            """CREATE TABLE vertices AS
SELECT row_number() OVER ()::UBIGINT AS __gql_id,
       CAST("external_id:ID(RoadNet)" AS VARCHAR) AS __gql_external_id,
       lower(trim(":LABEL")) AS __gql_label,
       CAST("external_id:ID(RoadNet)" AS VARCHAR) AS external_id
FROM raw_nodes""",
        ),
        (
            "edge_load",
            """CREATE TABLE edges AS
SELECT row_number() OVER ()::UBIGINT AS __gql_edge_id,
       source.__gql_id AS __gql_source_id,
       target.__gql_id AS __gql_target_id,
       lower(trim(raw.":TYPE")) AS __gql_type
FROM raw_edges raw
LEFT JOIN vertices source
  ON CAST(raw.":START_ID(RoadNet)" AS VARCHAR) = source.__gql_external_id
LEFT JOIN vertices target
  ON CAST(raw.":END_ID(RoadNet)" AS VARCHAR) = target.__gql_external_id""",
        ),
    ]


def copy_graph_sql(nodes: Path, edges: Path, validate: bool) -> str:
    validation = "TRUE" if validate else "FALSE"
    return f"""
COPY GRAPH roadnet FROM (
    VERTICES {sql_literal(nodes)},
    EDGES {sql_literal(edges)}
) FORMAT NEO4J OPTIONS (VALIDATE {validation})
"""


def workload_sql(case: str, nodes: Path, edges: Path, threads: int) -> str:
    sql = f"PRAGMA threads={threads};\n"
    if case != "native_tables":
        sql += "CREATE GRAPH roadnet ANY;\n"
    sql += ".timer on\n"
    if case == "native_tables":
        for name, statement in native_load_statements(nodes, edges):
            sql += marked_statement(name, statement)
    elif case == "copy_graph_fast":
        sql += marked_statement("load", copy_graph_sql(nodes, edges, False))
    elif case == "copy_graph_strict":
        sql += marked_statement("load", copy_graph_sql(nodes, edges, True))
    else:
        raise ValueError(f"unknown case: {case}")
    sql += marked_statement("checkpoint", "CHECKPOINT")
    sql += ".timer off\n"
    if case == "native_tables":
        sql += (
            f"SELECT '{RESULT_MARKER}', (SELECT count(*) FROM vertices), "
            "(SELECT count(*) FROM edges);\n"
        )
    else:
        sql += (
            f"SELECT '{RESULT_MARKER}', vertex_count, edge_count "
            "FROM gql_graphs() WHERE graph_name='roadnet';\n"
        )
    return sql


def timed_command(cli: Path, extension: Path, database: Path, graph: bool) -> list[str]:
    flag = "-l" if platform.system() == "Darwin" else "-v"
    command = [
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
    if graph:
        command += ["-cmd", f"LOAD {sql_literal(extension)};"]
    return command


def parse_phases(stdout: str) -> dict[str, float]:
    pending: str | None = None
    phases: dict[str, float] = {}
    for line in stdout.splitlines():
        if line.startswith(PHASE_MARKER):
            pending = line[len(PHASE_MARKER) :].strip()
            continue
        match = TIMER_RE.search(line)
        if match and pending is not None:
            phases[pending] = float(match.group(1))
            pending = None
    if "checkpoint" not in phases or len(phases) < 2:
        raise RuntimeError(f"unexpected benchmark phases: {phases}")
    return phases


def parse_counts(stdout: str) -> tuple[int, int]:
    for line in stdout.splitlines():
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


def run_trial(
    case: str,
    cli: Path,
    extension: Path,
    nodes: Path,
    edges: Path,
    threads: int,
    trial: int,
    warmup: bool,
) -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix=f"duckdb-gql-copy-{case}-") as temp:
        database = Path(temp) / "roadnet.duckdb"
        started = time.perf_counter()
        completed = subprocess.run(
            timed_command(cli, extension, database, case != "native_tables"),
            input=workload_sql(case, nodes, edges, threads),
            text=True,
            capture_output=True,
            check=False,
        )
        process_wall = time.perf_counter() - started
        if completed.returncode:
            raise RuntimeError(
                f"{case} failed\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
            )
        phases = parse_phases(completed.stdout)
        counts = parse_counts(completed.stdout)
        database_bytes = database.stat().st_size
        wal = Path(str(database) + ".wal")
        if wal.exists():
            database_bytes += wal.stat().st_size
        return {
            "case": case,
            "trial": trial,
            "warmup": warmup,
            "load_seconds": sum(value for name, value in phases.items() if name != "checkpoint"),
            "checkpoint_seconds": phases["checkpoint"],
            "process_wall_seconds": process_wall,
            "database_bytes": database_bytes,
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


def summarize_trials(trials: list[dict[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    graph_rows = trials[0]["vertex_rows"] + trials[0]["edge_rows"]
    for case in sorted({trial["case"] for trial in trials}):
        rows = [trial for trial in trials if trial["case"] == case and not trial["warmup"]]
        result[case] = {
            "runs": len(rows),
            "load_seconds": summarize([row["load_seconds"] for row in rows]),
            "checkpoint_seconds": summarize([row["checkpoint_seconds"] for row in rows]),
            "process_wall_seconds": summarize([row["process_wall_seconds"] for row in rows]),
            "database_bytes": summarize([row["database_bytes"] for row in rows]),
            "peak_rss_bytes": summarize([row["peak_rss_bytes"] for row in rows if row["peak_rss_bytes"]]),
        }
    native = result["native_tables"]["load_seconds"]["median"]
    for entry in result.values():
        load = entry["load_seconds"]["median"]
        entry["graph_rows_per_second"] = graph_rows / load
        entry["load_slowdown_vs_native"] = load / native
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
        "--source", type=Path, default=Path("data/roadnet-ca/roadNet-CA.txt.gz")
    )
    parser.add_argument(
        "--prepared-dir", type=Path, default=Path("data/roadnet-ca/neo4j")
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--threads", type=int, default=15)
    parser.add_argument("--dataset-name", default="roadNet-CA")
    parser.add_argument("--skip-rows", type=int, default=4)
    parser.add_argument("--vertex-label", default="intersection")
    parser.add_argument("--edge-type", default="road")
    parser.add_argument("--expected-vertices", type=int, default=1965206)
    parser.add_argument("--expected-edges", type=int, default=5533214)
    args = parser.parse_args()

    cli = args.duckdb.resolve()
    extension = args.extension.resolve()
    source = args.source.resolve()
    for path in (cli, extension, source):
        if not path.exists():
            raise SystemExit(f"missing required path: {path}")
    if args.runs < 1 or args.warmups < 0:
        raise SystemExit("runs must be positive and warmups non-negative")
    nodes, edges = prepare_inputs(
        cli,
        source,
        args.prepared_dir.resolve(),
        args.threads,
        args.skip_rows,
        args.vertex_label,
        args.edge_type,
    )

    cases = ["native_tables", "copy_graph_fast", "copy_graph_strict"]
    randomizer = random.Random(20260720)
    trials: list[dict[str, Any]] = []
    for trial in range(args.warmups + args.runs):
        order = cases.copy()
        randomizer.shuffle(order)
        for case in order:
            result = run_trial(
                case,
                cli,
                extension,
                nodes,
                edges,
                args.threads,
                trial,
                trial < args.warmups,
            )
            if result["vertex_rows"] != args.expected_vertices:
                raise RuntimeError(
                    f"expected {args.expected_vertices} vertices, found {result['vertex_rows']}"
                )
            if result["edge_rows"] != args.expected_edges:
                raise RuntimeError(
                    f"expected {args.expected_edges} edges, found {result['edge_rows']}"
                )
            trials.append(result)

    payload = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "threads": args.threads,
        "runs": args.runs,
        "warmups": args.warmups,
        "dataset": {
            "name": f"{args.dataset_name} Neo4j CSV pair",
            "source": str(source),
            "vertices": str(nodes),
            "edges": str(edges),
            "input_bytes": nodes.stat().st_size + edges.stat().st_size,
            "vertex_rows": args.expected_vertices,
            "edge_rows": args.expected_edges,
        },
        "trials": trials,
        "summary": summarize_trials(trials),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n")
    print(json.dumps({"output": str(args.output), "summary": payload["summary"]}, indent=2))


if __name__ == "__main__":
    main()
