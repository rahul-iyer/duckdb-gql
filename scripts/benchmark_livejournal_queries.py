#!/usr/bin/env python3
"""Profile native SQL and table-backed GQL queries on managed LiveJournal tables."""

from __future__ import annotations

import argparse
import json
import platform
import re
import statistics
import subprocess
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


RSS_RE = re.compile(r"^\s*(\d+)\s+maximum resident set size\s*$", re.MULTILINE)
GNU_RSS_RE = re.compile(r"Maximum resident set size \(kbytes\):\s*(\d+)")


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


def peak_rss_bytes(stderr: str) -> int | None:
    match = RSS_RE.search(stderr)
    if match:
        return int(match.group(1))
    match = GNU_RSS_RE.search(stderr)
    return int(match.group(1)) * 1024 if match else None


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


def gql_install_mode(cli: Path) -> str:
    completed = subprocess.run(
        [
            str(cli),
            "-csv",
            "-noheader",
            "-no-init",
            "-c",
            "SELECT install_mode FROM duckdb_extensions() "
            "WHERE extension_name = 'gql';",
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(f"failed to inspect GQL extension mode: {completed.stderr}")
    return completed.stdout.strip()


def native_one_hop(seed: str) -> str:
    return f"""
SELECT count(*)
FROM gql_data.graph_1_vertices source
JOIN gql_data.graph_1_edges edge
  ON edge.__gql_source_id = source.__gql_id
JOIN gql_data.graph_1_vertices target
  ON target.__gql_id = edge.__gql_target_id
WHERE source.external_id = {sql_literal(seed)}
  AND source.__gql_label = 'user'
  AND edge.__gql_type = 'follows'
  AND target.__gql_label = 'user';
"""


def gql_one_hop(seed: str) -> str:
    return f"""
MATCH (source:user)-[:follows]->(target:user)
WHERE source.external_id = {sql_literal(seed)}
RETURN count(*);
"""


def native_two_hop(seed: str) -> str:
    return f"""
SELECT count(*)
FROM gql_data.graph_1_vertices source
JOIN gql_data.graph_1_edges edge1
  ON edge1.__gql_source_id = source.__gql_id
JOIN gql_data.graph_1_vertices middle
  ON middle.__gql_id = edge1.__gql_target_id
JOIN gql_data.graph_1_edges edge2
  ON edge2.__gql_source_id = middle.__gql_id
JOIN gql_data.graph_1_vertices target
  ON target.__gql_id = edge2.__gql_target_id
WHERE source.external_id = {sql_literal(seed)}
  AND source.__gql_label = 'user'
  AND edge1.__gql_type = 'follows'
  AND middle.__gql_label = 'user'
  AND edge2.__gql_type = 'follows'
  AND target.__gql_label = 'user'
  AND edge1.__gql_edge_id <> edge2.__gql_edge_id;
"""


def gql_two_hop(seed: str) -> str:
    return f"""
MATCH (source:user)-[:follows]->{{2}}(target:user)
WHERE source.external_id = {sql_literal(seed)}
RETURN count(*);
"""


def native_recursive(seed: str, result_paths: int) -> str:
    return f"""
WITH RECURSIVE paths(start_id, end_id, used_edges, depth) AS (
    SELECT source.__gql_id, source.__gql_id, []::VARCHAR[], 0::UBIGINT
    FROM gql_data.graph_1_vertices source
    WHERE source.external_id = {sql_literal(seed)}
      AND source.__gql_label = 'user'
    UNION ALL
    SELECT paths.start_id, edge.__gql_target_id,
           list_append(paths.used_edges, CAST(edge.__gql_edge_id AS VARCHAR)),
           paths.depth + 1
    FROM paths
    JOIN gql_data.graph_1_edges edge
      ON edge.__gql_source_id = paths.end_id
    WHERE edge.__gql_type = 'follows'
      AND NOT list_contains(paths.used_edges, CAST(edge.__gql_edge_id AS VARCHAR))
)
SELECT target.external_id
FROM paths
JOIN gql_data.graph_1_vertices target
  ON target.__gql_id = paths.end_id
WHERE target.__gql_label = 'user'
LIMIT {result_paths};
"""


def gql_recursive(seed: str, result_paths: int) -> str:
    return f"""
MATCH (source:user)-[:follows]->*(target:user)
WHERE source.external_id = {sql_literal(seed)}
RETURN target.external_id
LIMIT {result_paths};
"""


def profile_workload(
    name: str,
    query: str,
    prefix: str,
    graph: bool,
    result_paths: int,
    cli: Path,
    extension: Path,
    database: Path,
    threads: int,
    warmups: int,
    runs: int,
    include_operator_profiles: bool,
    load_extension: bool,
) -> dict[str, Any]:
    time_flag = "-l" if platform.system() == "Darwin" else "-v"
    command = [
        "/usr/bin/time",
        time_flag,
        str(cli),
        "-unsigned",
        "-no-init",
        str(database),
    ]
    if load_extension:
        command += ["-cmd", f"LOAD {sql_literal(extension)};"]
    command += ["-cmd", f"PRAGMA threads={threads};"]
    if graph:
        command += ["-cmd", "SESSION SET GRAPH livejournal;"]
    payload = ".output /dev/null\nPRAGMA enable_profiling='json';\n"
    payload += (query.rstrip() + "\n") * (warmups + runs)
    completed = subprocess.run(
        command, input=payload, text=True, capture_output=True, check=False
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
    latencies = [entry["latency"] for entry in measured]
    cpu_times = [entry["cpu_time"] for entry in measured]
    median = statistics.median(latencies)
    result = {
        "runs": runs,
        "warmups": warmups,
        "result_paths": result_paths,
        "latency_seconds": summarize(latencies),
        "cpu_seconds": summarize(cpu_times),
        "paths_per_second": result_paths / median,
        "process_peak_rss_bytes": peak_rss_bytes(completed.stderr),
    }
    if include_operator_profiles:
        result["operator_profiles"] = measured
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, default=Path("build/release/duckdb"))
    parser.add_argument(
        "--extension",
        type=Path,
        default=Path("build/release/extension/duckgql/duckgql.duckdb_extension"),
    )
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", default="10009")
    parser.add_argument("--one-hop-paths", type=int, default=20293)
    parser.add_argument("--two-hop-paths", type=int, default=855573)
    parser.add_argument("--recursive-paths", type=int, default=100)
    parser.add_argument(
        "--vlp-only",
        action="store_true",
        help="profile only the matched native SQL and GQL recursive workloads",
    )
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--threads", type=int, default=15)
    parser.add_argument(
        "--include-operator-profiles",
        action="store_true",
        help="retain complete measured DuckDB JSON operator trees in the output",
    )
    args = parser.parse_args()

    cli = args.duckdb.resolve()
    extension = args.extension.resolve()
    database = args.database.resolve()
    for path in (cli, database):
        if not path.exists():
            raise SystemExit(f"missing required path: {path}")
    install_mode = gql_install_mode(cli)
    statically_linked = install_mode == "STATICALLY_LINKED"
    if not statically_linked and not extension.exists():
        raise SystemExit(f"missing required path: {extension}")
    if statically_linked:
        source_root = Path(__file__).resolve().parents[1] / "src"
        gql_sources = list(source_root.glob("**/*.cpp")) + list(source_root.glob("**/*.hpp"))
        newest_source = max(path.stat().st_mtime for path in gql_sources)
        if cli.stat().st_mtime < newest_source:
            raise SystemExit(
                "DuckDB CLI contains a stale statically linked GQL build; run "
                "cmake --build build/release --target shell before benchmarking"
            )
    if args.runs < 1 or args.warmups < 0:
        raise SystemExit("runs must be positive and warmups non-negative")
    if args.recursive_paths < 1:
        raise SystemExit("recursive-paths must be positive")

    workloads = [
        ("native_one_hop_count", native_one_hop(args.seed), "SELECT count(*)", False, args.one_hop_paths),
        ("gql_one_hop_count", gql_one_hop(args.seed), "MATCH (source:user)", True, args.one_hop_paths),
        ("native_two_hop_count", native_two_hop(args.seed), "SELECT count(*)", False, args.two_hop_paths),
        ("gql_two_hop_count", gql_two_hop(args.seed), "MATCH (source:user)", True, args.two_hop_paths),
        (
            f"native_recursive_first_{args.recursive_paths}",
            native_recursive(args.seed, args.recursive_paths),
            "WITH RECURSIVE paths",
            False,
            args.recursive_paths,
        ),
        (
            f"gql_recursive_first_{args.recursive_paths}",
            gql_recursive(args.seed, args.recursive_paths),
            "MATCH (source:user)",
            True,
            args.recursive_paths,
        ),
    ]
    if args.vlp_only:
        workloads = workloads[-2:]
    results: dict[str, Any] = {}
    for name, query, prefix, graph, paths in workloads:
        results[name] = profile_workload(
            name,
            query,
            prefix,
            graph,
            paths,
            cli,
            extension,
            database,
            args.threads,
            args.warmups,
            args.runs,
            args.include_operator_profiles,
            not statically_linked,
        )

    payload = {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(),
        "machine": platform.machine(),
        "threads": args.threads,
        "runs": args.runs,
        "warmups": args.warmups,
        "gql_install_mode": install_mode,
        "dataset": {
            "name": "soc-LiveJournal1 managed COPY GRAPH database",
            "vertices": 4847571,
            "edges": 68993773,
            "seed": args.seed,
            "seed_out_degree": args.one_hop_paths,
            "recursive_result_paths": args.recursive_paths,
        },
        "query_summary": results,
        "unsupported": {
            "prepared_csr": "Not measured until the current materializing CSR builder is memory-safe at 69M edges",
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n")
    print(json.dumps({"output": str(args.output), "queries": results}, indent=2))


if __name__ == "__main__":
    main()
