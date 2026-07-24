#!/usr/bin/env python3
"""Run supported kernels on an official LDBC Graphalytics scale dataset."""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from benchmark_graphalytics import (
    BASE_URL,
    EPSILON,
    MARKER_PREFIX,
    SPEC_URL,
    parse_timers,
    sha256,
    sql_literal,
)


RSS_RE = re.compile(r"^\s*(\d+)\s+maximum resident set size\s*$", re.MULTILINE)
CSR_STATS_MARKER = "__DUCKGQL_CSR_STATS__"
UINT64_BYTES = 8
UINT32_BYTES = 4


@dataclass(frozen=True)
class DatasetConfig:
    name: str
    vertices: int
    edges: int
    directed: bool
    weighted: bool
    algorithms: tuple[str, ...]
    bfs_source: int
    pr_damping: float
    pr_iterations: int


def ensure_dataset(dataset: str, cache_dir: Path) -> tuple[Path, Path, str]:
    cache_dir.mkdir(parents=True, exist_ok=True)
    archive = cache_dir / f"{dataset}.tar.zst"
    if not archive.exists():
        archive_url = f"{BASE_URL}/{archive.name}"
        request = urllib.request.Request(
            archive_url,
            headers={"User-Agent": "DuckGQL-graphalytics/1.0"},
        )
        temporary = archive.with_suffix(archive.suffix + ".part")
        with urllib.request.urlopen(request) as source, temporary.open("wb") as output:
            shutil.copyfileobj(source, output)
        temporary.replace(archive)
    extracted = cache_dir / dataset
    if not (extracted / f"{dataset}.properties").exists():
        extracted.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            ["tar", "--use-compress-program=unzstd", "-xf", str(archive), "-C", str(extracted)],
            check=True,
        )
    return archive, extracted, sha256(archive)


def read_config(dataset: str, extracted: Path) -> DatasetConfig:
    properties: dict[str, str] = {}
    for line in (extracted / f"{dataset}.properties").read_text().splitlines():
        if "=" not in line or line.lstrip().startswith("#"):
            continue
        key, value = line.split("=", 1)
        properties[key.strip()] = value.strip()
    prefix = f"graph.{dataset}."
    return DatasetConfig(
        name=dataset,
        vertices=int(properties[prefix + "meta.vertices"]),
        edges=int(properties[prefix + "meta.edges"]),
        directed=properties[prefix + "directed"].lower() == "true",
        weighted="weight" in properties.get(prefix + "edge-properties.names", ""),
        algorithms=tuple(
            algorithm.strip().upper()
            for algorithm in properties[prefix + "algorithms"].split(",")
        ),
        bfs_source=int(properties[prefix + "bfs.source-vertex"]),
        pr_damping=float(properties[prefix + "pr.damping-factor"]),
        pr_iterations=int(properties[prefix + "pr.num-iterations"]),
    )


def prepare_inputs(
    cli: Path,
    config: DatasetConfig,
    extracted: Path,
    directory: Path,
    threads: int,
) -> tuple[Path, Path]:
    nodes = directory / "nodes.csv"
    edges = directory / "edges.csv"
    vertex_file = extracted / f"{config.name}.v"
    edge_file = extracted / f"{config.name}.e"
    edge_columns = (
        "{'source':'BIGINT','target':'BIGINT','weight':'DOUBLE'}"
        if config.weighted
        else "{'source':'BIGINT','target':'BIGINT'}"
    )
    edge_scan = (
        f"SELECT source, target FROM read_csv({sql_literal(edge_file)}, "
        f"delim=' ', header=false, columns={edge_columns})"
    )
    if not config.directed:
        edge_scan = (
            f"WITH logical_edges AS ({edge_scan}) "
            "SELECT source, target FROM logical_edges "
            "UNION ALL SELECT target, source FROM logical_edges"
        )
    sql = f"""
PRAGMA threads={threads};
COPY (
  SELECT id::VARCHAR AS "external_id:ID(Graphalytics)",
         id AS "graphalytics_id:long", 'vertex' AS ":LABEL"
  FROM read_csv({sql_literal(vertex_file)}, delim=' ', header=false,
                columns={{'id':'BIGINT'}})
) TO {sql_literal(nodes)} (FORMAT CSV, HEADER);
COPY (
  SELECT source::VARCHAR AS ":START_ID(Graphalytics)",
         target::VARCHAR AS ":END_ID(Graphalytics)", 'edge' AS ":TYPE"
  FROM ({edge_scan}) stored_arcs
) TO {sql_literal(edges)} (FORMAT CSV, HEADER);
"""
    completed = subprocess.run(
        [str(cli), "-unsigned", "-no-init", ":memory:"],
        input=sql,
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(f"input conversion failed: {completed.stderr}")
    return nodes, edges


def algorithm_queries(config: DatasetConfig) -> dict[str, str]:
    return {
        "BFS": (
            "MATCH (source:vertex) "
            f"WHERE source.graphalytics_id = {config.bfs_source} "
            "CALL algo.bfs('bench_graph', element_id(source)) "
            "YIELD vertex_id, depth "
            "RETURN vertex_id, depth"
        ),
        "PR": (
            "CALL algo.pagerank('bench_graph', "
            f"damping := {config.pr_damping}, "
            f"max_iterations := {config.pr_iterations}, tolerance := 1e-300) "
            "YIELD vertex_id, rank RETURN vertex_id, rank"
        ),
        "WCC": (
            "CALL algo.wcc('bench_graph') YIELD vertex_id, component_id "
            "RETURN vertex_id, component_id"
        ),
        "LCC": (
            "CALL algo.lcc('bench_graph') "
            "YIELD vertex_id, local_clustering_coefficient "
            "RETURN vertex_id, local_clustering_coefficient"
        ),
    }


def run_workloads(
    cli: Path,
    extension: Path,
    database: Path,
    nodes: Path,
    edges: Path,
    directory: Path,
    queries: dict[str, str],
    threads: int,
    warmups: int,
    runs: int,
) -> tuple[dict[str, float], dict[str, Path], int | None, dict[str, Any]]:
    statements = [
        f"LOAD {sql_literal(extension)};",
        f"PRAGMA threads={threads};",
        ".headers off",
        ".mode csv",
        "CREATE GRAPH bench_graph ANY;",
        ".timer on",
        f".print {MARKER_PREFIX}copy_graph",
        (
            "COPY GRAPH bench_graph FROM ("
            f"VERTICES {sql_literal(nodes)}, EDGES {sql_literal(edges)}"
            ") FORMAT GRAPH;"
        ),
        "SESSION SET GRAPH bench_graph;",
        f".print {MARKER_PREFIX}csr_build",
        "CALL gql_build_csr('bench_graph');",
        ".timer off",
        f".print {CSR_STATS_MARKER}",
        (
            "SELECT memory_bytes, topology_bytes, identity_bytes, label_bytes, "
            "auxiliary_bytes, build_auxiliary_bytes, neighbor_width_bytes, "
            "vertex_ids_explicit, edge_labels_uniform "
            "FROM gql_csr_stats('bench_graph');"
        ),
    ]
    validation_outputs: dict[str, Path] = {}
    for algorithm, query in queries.items():
        for index in range(warmups + runs):
            kind = "warmup" if index < warmups else "run"
            ordinal = index if index < warmups else index - warmups
            if kind == "run" and ordinal == 0:
                output = directory / f"actual-{algorithm}.csv"
                validation_outputs[algorithm] = output
            else:
                output = Path("/dev/null")
            statements.extend(
                [
                    ".timer on",
                    f".print {MARKER_PREFIX}{algorithm}_{kind}_{ordinal}",
                    f".output {output}",
                    query + ";",
                    ".output stdout",
                    ".timer off",
                ]
            )
    command = [str(cli), "-unsigned", "-no-init", str(database)]
    if Path("/usr/bin/time").exists():
        command = ["/usr/bin/time", "-l", *command]
    completed = subprocess.run(
        command,
        input="\n".join(statements) + "\n",
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(
            f"scale workload failed\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    timers = parse_timers(completed.stdout + "\n" + completed.stderr)
    rss_match = RSS_RE.search(completed.stderr)
    peak_rss = int(rss_match.group(1)) if rss_match else None
    lines = completed.stdout.splitlines()
    try:
        marker_index = lines.index(CSR_STATS_MARKER)
        fields = lines[marker_index + 1].split(",")
    except (ValueError, IndexError) as error:
        raise RuntimeError("CSR memory statistics were not emitted") from error
    if len(fields) != 9:
        raise RuntimeError(f"unexpected CSR memory statistics: {fields}")
    csr_stats = {
        "memory_bytes": int(fields[0]),
        "topology_bytes": int(fields[1]),
        "identity_bytes": int(fields[2]),
        "label_bytes": int(fields[3]),
        "auxiliary_bytes": int(fields[4]),
        "build_auxiliary_bytes": int(fields[5]),
        "neighbor_width_bytes": int(fields[6]),
        "vertex_ids_explicit": fields[7].lower() == "true",
        "edge_labels_uniform": fields[8].lower() == "true",
    }
    return timers, validation_outputs, peak_rss, csr_stats


def process_peak_rss(
    cli: Path, extension: Path, database: Path, statement: str
) -> int | None:
    command = [str(cli), "-unsigned", "-no-init", str(database)]
    if Path("/usr/bin/time").exists():
        command = ["/usr/bin/time", "-l", *command]
    completed = subprocess.run(
        command,
        input=(
            f"LOAD {sql_literal(extension)};\n"
            ".output /dev/null\n"
            f"{statement}\n"
        ),
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(
            f"RSS measurement failed\nstdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    match = RSS_RE.search(completed.stderr)
    return int(match.group(1)) if match else None


def validation_query(algorithm: str, actual: Path, reference: Path) -> str:
    actual_type = "DOUBLE" if algorithm in {"PR", "LCC"} else "BIGINT"
    actual_relation = (
        f"read_csv({sql_literal(actual)}, header=false, "
        f"columns={{'internal_id':'UBIGINT','value':'{actual_type}'}})"
    )
    reference_relation = (
        f"read_csv({sql_literal(reference)}, delim=' ', header=false, "
        f"columns={{'vertex_id':'BIGINT','value':'{actual_type}'}})"
    )
    if algorithm == "BFS":
        actual_cte = (
            "actual AS (SELECT v.graphalytics_id AS vertex_id, "
            "coalesce(a.value, 9223372036854775807) AS value "
            "FROM gql_data.graph_1_vertices v LEFT JOIN "
            f"{actual_relation} a ON a.internal_id = v.__gql_id)"
        )
        predicate = "a.value IS DISTINCT FROM r.value"
        body = (
            f"WITH {actual_cte}, reference AS (SELECT * FROM {reference_relation}) "
            "SELECT count(*) FILTER (WHERE " + predicate + ")::BIGINT, 0.0 "
            "FROM actual a FULL OUTER JOIN reference r USING (vertex_id)"
        )
    elif algorithm in {"PR", "LCC"}:
        body = f"""
WITH actual AS (
  SELECT v.graphalytics_id AS vertex_id, a.value
  FROM {actual_relation} a
  JOIN gql_data.graph_1_vertices v ON a.internal_id = v.__gql_id
), reference AS (SELECT * FROM {reference_relation}), compared AS (
  SELECT r.vertex_id, r.value AS reference_value, a.value AS actual_value,
         CASE WHEN r.value = 0 THEN abs(a.value-r.value)
              ELSE abs(a.value-r.value)/abs(r.value) END AS relative_error
  FROM reference r FULL OUTER JOIN actual a USING (vertex_id)
)
SELECT count(*) FILTER (
         WHERE actual_value IS NULL OR reference_value IS NULL
            OR abs(actual_value-reference_value) > {EPSILON}*abs(reference_value)
       )::BIGINT,
       coalesce(max(relative_error), 0.0)
FROM compared
"""
    elif algorithm == "WCC":
        body = f"""
WITH actual AS (
  SELECT v.graphalytics_id AS vertex_id, a.value
  FROM {actual_relation} a
  JOIN gql_data.graph_1_vertices v ON a.internal_id = v.__gql_id
), reference AS (SELECT * FROM {reference_relation}),
forward_bad AS (
  SELECT count(*) AS failures FROM (
    SELECT r.value FROM reference r JOIN actual a USING (vertex_id)
    GROUP BY r.value HAVING count(DISTINCT a.value) != 1
  )
), reverse_bad AS (
  SELECT count(*) AS failures FROM (
    SELECT a.value FROM reference r JOIN actual a USING (vertex_id)
    GROUP BY a.value HAVING count(DISTINCT r.value) != 1
  )
), vertex_bad AS (
  SELECT abs((SELECT count(*) FROM actual) - (SELECT count(*) FROM reference)) AS failures
)
SELECT (forward_bad.failures + reverse_bad.failures + vertex_bad.failures)::BIGINT, 0.0
FROM forward_bad, reverse_bad, vertex_bad
"""
    else:
        raise ValueError(algorithm)
    return body


def validate(
    cli: Path,
    config: DatasetConfig,
    database: Path,
    extracted: Path,
    outputs: dict[str, Path],
) -> dict[str, dict[str, Any]]:
    results: dict[str, dict[str, Any]] = {}
    for algorithm, actual in outputs.items():
        query = validation_query(
            algorithm, actual, extracted / f"{config.name}-{algorithm}"
        )
        completed = subprocess.run(
            [str(cli), "-unsigned", "-no-init", "-csv", "-noheader", str(database), "-c", query],
            text=True,
            capture_output=True,
            check=False,
        )
        if completed.returncode:
            raise RuntimeError(f"{algorithm} validation failed: {completed.stderr}")
        failures_text, error_text = completed.stdout.strip().split(",")
        failures = int(failures_text)
        results[algorithm] = {
            "passed": failures == 0,
            "failure_count": failures,
            "method": (
                "exact"
                if algorithm == "BFS"
                else "equivalence" if algorithm == "WCC" else "epsilon"
            ),
            "epsilon": EPSILON if algorithm in {"PR", "LCC"} else None,
            "maximum_relative_error": float(error_text),
        }
    return results


def summary(values: list[float]) -> dict[str, float]:
    return {
        "minimum": min(values),
        "median": statistics.median(values),
        "maximum": max(values),
        "mean": statistics.fmean(values),
    }


def markdown(result: dict[str, Any]) -> str:
    dataset = result["dataset"]
    direction = "directed" if dataset["directed"] else "undirected"
    lines = [
        f"# LDBC Graphalytics {dataset['name']} local run",
        "",
        f"Generated: {result['generated_at']}",
        "",
        "Local run over the official dataset and reference outputs; this is "
        "not an audited Graphalytics submission.",
        "",
        f"- Graph: {result['dataset']['vertices']:,} vertices / "
        f"{result['dataset']['edges']:,} {direction} logical edges",
        f"- Stored CSR arcs: {dataset['stored_csr_arcs']:,}",
        f"- Threads: {result['configuration']['threads']}",
        f"- COPY GRAPH: {result['timing_seconds']['copy_graph']:.3f}s",
        f"- CSR build: {result['timing_seconds']['csr_build']:.3f}s",
        f"- Accounted CSR memory: {result['csr']['memory_bytes'] / (1024**3):.2f} GiB",
        (
            f"- Reduction from legacy principal-array layout: "
            f"{result['csr']['reduction_from_legacy_percent']:.1f}%"
        ),
        f"- Neighbor ordinal width: {result['csr']['neighbor_width_bytes']} bytes",
        (
            f"- Peak process RSS: "
            f"{result['environment']['peak_rss_bytes'] / (1024**3):.2f} GiB"
            if result["environment"]["peak_rss_bytes"]
            else "- Peak process RSS: unavailable"
        ),
        (
            f"- CSR-build-only peak RSS: "
            f"{result['csr']['build_process_peak_rss_bytes'] / (1024**3):.2f} GiB"
            if result["csr"]["build_process_peak_rss_bytes"]
            else "- CSR-build-only peak RSS: unavailable"
        ),
        (
            f"- CSR-build incremental peak above an idle database: "
            f"{result['csr']['build_process_incremental_peak_rss_bytes'] / (1024**3):.2f} GiB"
            if result["csr"]["build_process_incremental_peak_rss_bytes"]
            else "- CSR-build incremental peak above an idle database: unavailable"
        ),
        (
            f"- CSR-build accounting coverage: "
            f"{result['csr']['build_peak_accounting_coverage_percent']:.1f}%"
            if result["csr"]["build_peak_accounting_coverage_percent"]
            else "- CSR-build accounting coverage: unavailable"
        ),
        "",
        "| Kernel | Validation | Warm median | Min | Max |",
        "|---|---:|---:|---:|---:|",
    ]
    for algorithm, timing in result["timing_seconds"]["algorithms"].items():
        status = "PASS" if result["validation"][algorithm]["passed"] else "FAIL"
        lines.append(
            f"| {algorithm} | {status} | {timing['median']:.3f}s | "
            f"{timing['minimum']:.3f}s | {timing['maximum']:.3f}s |"
        )
    lines.extend(
        [
            "",
            "CDLP is not implemented. The current DuckGQL SSSP is "
            "unweighted and is not run against Graphalytics weighted SSSP.",
            "",
            "## Reproduce",
            "",
            "```sh",
            result["reproduce"],
            "```",
            "",
        ]
    )
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", default="wiki-Talk")
    parser.add_argument("--cli", type=Path, default=Path("build/release/duckdb"))
    parser.add_argument(
        "--extension",
        type=Path,
        default=Path("build/release/extension/duckgql/duckgql.duckdb_extension"),
    )
    parser.add_argument(
        "--cache-dir", type=Path, default=Path("build/benchmarks/graphalytics-data")
    )
    parser.add_argument(
        "--output",
        type=Path,
    )
    parser.add_argument(
        "--report",
        type=Path,
    )
    parser.add_argument("--threads", type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--runs", type=int, default=3)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    cli = args.cli.resolve()
    extension = args.extension.resolve()
    archive, extracted, archive_sha = ensure_dataset(
        args.dataset, args.cache_dir.resolve()
    )
    config = read_config(args.dataset, extracted)
    queries = algorithm_queries(config)
    print(f"[{config.name}] official archive {archive_sha}", flush=True)
    with tempfile.TemporaryDirectory(prefix="duckgql-graphalytics-scale-") as temporary:
        directory = Path(temporary)
        print(f"[{config.name}] converting official text input", flush=True)
        nodes, edges = prepare_inputs(
            cli, config, extracted, directory, args.threads
        )
        database = directory / "graphalytics.duckdb"
        print(
            f"[{config.name}] loading graph, building CSR, and running kernels",
            flush=True,
        )
        timers, outputs, peak_rss, csr_stats = run_workloads(
            cli,
            extension,
            database,
            nodes,
            edges,
            directory,
            queries,
            args.threads,
            args.warmups,
            args.runs,
        )
        baseline_peak_rss = process_peak_rss(
            cli, extension, database, "SELECT 1;"
        )
        csr_build_peak_rss = process_peak_rss(
            cli, extension, database, "CALL gql_build_csr('bench_graph');"
        )
        print(
            f"[{config.name}] validating against official reference outputs",
            flush=True,
        )
        validation = validate(cli, config, database, extracted, outputs)
    algorithms = {
        algorithm: summary(
            [timers[f"{algorithm}_run_{index}"] for index in range(args.runs)]
        )
        for algorithm in queries
    }
    stored_csr_arcs = config.edges if config.directed else config.edges * 2
    legacy_principal_bytes = (
        config.vertices * UINT64_BYTES
        + (config.vertices + 1) * UINT64_BYTES
        + config.vertices * UINT32_BYTES
        + 2
        * (
            (config.vertices + 1) * UINT64_BYTES
            + stored_csr_arcs * UINT64_BYTES
            + stored_csr_arcs * UINT64_BYTES
            + stored_csr_arcs * UINT32_BYTES
        )
    )
    csr_stats["legacy_principal_bytes"] = legacy_principal_bytes
    csr_stats["reduction_from_legacy_percent"] = (
        100.0 * (legacy_principal_bytes - csr_stats["memory_bytes"])
        / legacy_principal_bytes
    )
    csr_stats["build_process_baseline_peak_rss_bytes"] = baseline_peak_rss
    csr_stats["build_process_peak_rss_bytes"] = csr_build_peak_rss
    csr_stats["build_process_incremental_peak_rss_bytes"] = (
        csr_build_peak_rss - baseline_peak_rss
        if csr_build_peak_rss is not None and baseline_peak_rss is not None
        else None
    )
    csr_stats["accounted_build_peak_bytes"] = (
        csr_stats["memory_bytes"] + csr_stats["build_auxiliary_bytes"]
    )
    incremental_peak = csr_stats["build_process_incremental_peak_rss_bytes"]
    csr_stats["build_peak_accounting_coverage_percent"] = (
        100.0 * csr_stats["accounted_build_peak_bytes"] / incremental_peak
        if incremental_peak
        else None
    )
    reproduce = (
        "python3 scripts/benchmark/benchmark_graphalytics_scale.py "
        f"--dataset {config.name} --threads {args.threads} "
        f"--warmups {args.warmups} --runs {args.runs}"
    )
    result: dict[str, Any] = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "qualification": "local compatibility and scale run; not an audited submission",
        "specification": SPEC_URL,
        "dataset": {
            "name": config.name,
            "vertices": config.vertices,
            "edges": config.edges,
            "directed": config.directed,
            "weighted": config.weighted,
            "stored_csr_arcs": stored_csr_arcs,
            "archive_url": f"{BASE_URL}/{archive.name}",
            "archive_sha256": archive_sha,
        },
        "environment": {
            "platform": platform.platform(),
            "cli": str(cli),
            "extension": str(extension),
            "peak_rss_bytes": peak_rss,
        },
        "configuration": {"threads": args.threads, "warmups": args.warmups, "runs": args.runs},
        "timing_seconds": {
            "copy_graph": timers["copy_graph"],
            "csr_build": timers["csr_build"],
            "algorithms": algorithms,
        },
        "csr": csr_stats,
        "validation": validation,
        "unsupported": ["CDLP", "weighted SSSP"],
        "reproduce": reproduce,
    }
    slug = config.name.lower().replace("_", "-")
    output = args.output or Path(
        f"build/benchmarks/graphalytics-{slug}-current.json"
    )
    report = args.report or Path(f"docs/benchmarks/graphalytics-{slug}.md")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(markdown(result))
    for algorithm, status in validation.items():
        print(
            f"[{algorithm}] {'PASS' if status['passed'] else 'FAIL'}; "
            f"median {algorithms[algorithm]['median']:.3f}s"
        )
    print(f"wrote {output}")
    print(f"wrote {report}")
    return 0 if all(item["passed"] for item in validation.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
