#!/usr/bin/env python3
"""Run DuckGQL against the official LDBC Graphalytics test datasets.

This is a local compatibility and timing harness, not an audited Graphalytics
submission.  It downloads the official test archives, loads each graph through
COPY GRAPH, builds the connection-local CSR explicitly, executes the public GQL
CALL surface, and validates the results against the bundled reference output.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
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


BASE_URL = "https://datasets.ldbcouncil.org/graphalytics"
SPEC_URL = "https://ldbcouncil.org/ldbc_graphalytics_docs/graphalytics_spec.pdf"
INFINITY = 9223372036854775807
EPSILON = 0.0001
TIMER_RE = re.compile(r"Run Time \(s\): real ([0-9.]+)")
MARKER_PREFIX = "__GRAPHALYTICS_PHASE__"


@dataclass(frozen=True)
class TestCase:
    name: str
    algorithm: str
    directed: bool
    source: int | None = None
    damping: float | None = None
    iterations: int | None = None


TEST_CASES = (
    TestCase("test-bfs-directed", "BFS", True, source=1),
    TestCase("test-bfs-undirected", "BFS", False, source=1),
    TestCase("test-pr-directed", "PR", True, damping=0.85, iterations=14),
    TestCase("test-pr-undirected", "PR", False, damping=0.85, iterations=26),
    TestCase("test-lcc-directed", "LCC", True),
    TestCase("test-lcc-undirected", "LCC", False),
    TestCase("test-wcc-directed", "WCC", True),
    TestCase("test-wcc-undirected", "WCC", False),
)


def sql_literal(value: str | Path) -> str:
    return "'" + str(value).replace("'", "''") + "'"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(case: TestCase, cache_dir: Path) -> tuple[Path, str]:
    cache_dir.mkdir(parents=True, exist_ok=True)
    archive = cache_dir / f"{case.name}.tar.zst"
    if not archive.exists():
        temporary = archive.with_suffix(archive.suffix + ".part")
        request = urllib.request.Request(
            f"{BASE_URL}/{archive.name}",
            headers={"User-Agent": "DuckGQL-graphalytics/1.0"},
        )
        with urllib.request.urlopen(request) as source, temporary.open("wb") as output:
            shutil.copyfileobj(source, output)
        temporary.replace(archive)
    return archive, sha256(archive)


def extract_archive(archive: Path, destination: Path) -> Path:
    destination.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        ["tar", "--use-compress-program=unzstd", "-xf", str(archive), "-C", str(destination)],
        check=True,
        capture_output=True,
        text=True,
    )
    return destination


def read_ids(path: Path) -> list[int]:
    return [int(line) for line in path.read_text().splitlines() if line.strip()]


def read_edges(path: Path) -> list[tuple[int, int]]:
    edges: list[tuple[int, int]] = []
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        source, target, *_ = line.split()
        edges.append((int(source), int(target)))
    return edges


def read_reference(path: Path, floating: bool) -> dict[int, int | float]:
    result: dict[int, int | float] = {}
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        vertex, value = line.split()[:2]
        result[int(vertex)] = float(value) if floating else int(value)
    return result


def write_graph_inputs(
    case: TestCase, extracted: Path, destination: Path
) -> tuple[Path, Path, list[int], int, int]:
    vertices = read_ids(extracted / f"{case.name}.v")
    logical_edges = read_edges(extracted / f"{case.name}.e")
    nodes_csv = destination / "nodes.csv"
    edges_csv = destination / "edges.csv"
    with nodes_csv.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow(["external_id:ID(Graphalytics)", "graphalytics_id:long", ":LABEL"])
        writer.writerows((vertex, vertex, "vertex") for vertex in vertices)
    stored_edges = list(logical_edges)
    if not case.directed:
        stored_edges.extend((target, source) for source, target in logical_edges)
    with edges_csv.open("w", newline="") as output:
        writer = csv.writer(output)
        writer.writerow([":START_ID(Graphalytics)", ":END_ID(Graphalytics)", ":TYPE"])
        writer.writerows((source, target, "edge") for source, target in stored_edges)
    return nodes_csv, edges_csv, vertices, len(logical_edges), len(stored_edges)


def gql_query(case: TestCase) -> str:
    if case.algorithm == "BFS":
        return (
            f"CALL algo.bfs('bench_graph', {case.source}) "
            "YIELD vertex_id, depth RETURN vertex_id, depth"
        )
    if case.algorithm == "PR":
        return (
            "CALL algo.pagerank('bench_graph', "
            f"damping := {case.damping}, max_iterations := {case.iterations}, "
            "tolerance := 1e-300) "
            "YIELD vertex_id, rank RETURN vertex_id, rank"
        )
    if case.algorithm == "WCC":
        return (
            "CALL algo.wcc('bench_graph') YIELD vertex_id, component_id "
            "RETURN vertex_id, component_id"
        )
    if case.algorithm == "LCC":
        return (
            "CALL algo.lcc('bench_graph') "
            "YIELD vertex_id, local_clustering_coefficient "
            "RETURN vertex_id, local_clustering_coefficient"
        )
    raise ValueError(f"unsupported algorithm: {case.algorithm}")


def parse_timers(stdout: str) -> dict[str, float]:
    pending: str | None = None
    phases: dict[str, float] = {}
    for line in stdout.splitlines():
        if line.startswith(MARKER_PREFIX):
            pending = line[len(MARKER_PREFIX) :].strip()
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
    extension: Path,
    case: TestCase,
    nodes_csv: Path,
    edges_csv: Path,
    directory: Path,
    threads: int,
    warmups: int,
    runs: int,
) -> tuple[dict[str, float], Path, Path]:
    mapping_csv = directory / "mapping.csv"
    query = gql_query(case)
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
            f"VERTICES {sql_literal(nodes_csv)}, EDGES {sql_literal(edges_csv)}"
            ") FORMAT GRAPH;"
        ),
        f".print {MARKER_PREFIX}csr_build",
        "CALL gql_build_csr('bench_graph');",
        ".timer off",
        f"COPY (SELECT __gql_id, graphalytics_id FROM gql_data.graph_1_vertices) "
        f"TO {sql_literal(mapping_csv)} (FORMAT CSV, HEADER FALSE);",
    ]
    output_paths: list[Path] = []
    for index in range(warmups + runs):
        kind = "warmup" if index < warmups else "run"
        ordinal = index if index < warmups else index - warmups
        output_path = directory / f"{kind}-{ordinal}.csv"
        output_paths.append(output_path)
        statements.extend(
            [
                ".timer on",
                f".print {MARKER_PREFIX}{kind}_{ordinal}",
                f".output {output_path}",
                query + ";",
                ".output stdout",
                ".timer off",
            ]
        )
    completed = subprocess.run(
        [str(cli), "-unsigned", "-no-init", ":memory:"],
        input="\n".join(statements) + "\n",
        text=True,
        capture_output=True,
        check=False,
    )
    if completed.returncode:
        raise RuntimeError(
            f"{case.name} failed\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    timers = parse_timers(completed.stdout + "\n" + completed.stderr)
    expected = {"copy_graph", "csr_build"}
    expected.update(f"warmup_{index}" for index in range(warmups))
    expected.update(f"run_{index}" for index in range(runs))
    missing = expected.difference(timers)
    if missing:
        raise RuntimeError(f"{case.name}: missing phase timers: {sorted(missing)}")
    return timers, mapping_csv, output_paths[-1]


def read_mapping(path: Path) -> dict[int, int]:
    with path.open(newline="") as source:
        return {int(internal): int(external) for internal, external in csv.reader(source)}


def read_actual(path: Path, mapping: dict[int, int], floating: bool) -> dict[int, int | float]:
    result: dict[int, int | float] = {}
    with path.open(newline="") as source:
        for internal, value in csv.reader(source):
            external = mapping[int(internal)]
            result[external] = float(value) if floating else int(value)
    return result


def epsilon_validation(
    reference: dict[int, int | float], actual: dict[int, int | float]
) -> dict[str, Any]:
    failures: list[dict[str, float | int]] = []
    maximum_relative_error = 0.0
    maximum_absolute_error = 0.0
    for vertex, reference_value_raw in reference.items():
        reference_value = float(reference_value_raw)
        if vertex not in actual:
            failures.append({"vertex_id": vertex, "reason": "missing"})
            continue
        actual_value = float(actual[vertex])
        absolute_error = abs(reference_value - actual_value)
        relative_error = (
            absolute_error / abs(reference_value)
            if reference_value
            else absolute_error
        )
        maximum_absolute_error = max(maximum_absolute_error, absolute_error)
        maximum_relative_error = max(maximum_relative_error, relative_error)
        if absolute_error > EPSILON * abs(reference_value):
            failures.append(
                {
                    "vertex_id": vertex,
                    "reference": reference_value,
                    "actual": actual_value,
                    "relative_error": relative_error,
                }
            )
    for vertex in actual.keys() - reference.keys():
        failures.append({"vertex_id": vertex, "reason": "unexpected"})
    return {
        "passed": not failures,
        "method": "epsilon",
        "epsilon": EPSILON,
        "maximum_absolute_error": maximum_absolute_error,
        "maximum_relative_error": maximum_relative_error,
        "failure_count": len(failures),
        "failures": failures[:10],
    }


def exact_validation(
    reference: dict[int, int | float], actual: dict[int, int | float]
) -> dict[str, Any]:
    failures = []
    for vertex in sorted(reference.keys() | actual.keys()):
        if reference.get(vertex) != actual.get(vertex):
            failures.append(
                {
                    "vertex_id": vertex,
                    "reference": reference.get(vertex),
                    "actual": actual.get(vertex),
                }
            )
    return {
        "passed": not failures,
        "method": "exact",
        "failure_count": len(failures),
        "failures": failures[:10],
    }


def equivalence_validation(
    reference: dict[int, int | float], actual: dict[int, int | float]
) -> dict[str, Any]:
    failures: list[dict[str, Any]] = []
    if reference.keys() != actual.keys():
        failures.append(
            {
                "reason": "vertex_set_mismatch",
                "missing": sorted(reference.keys() - actual.keys()),
                "unexpected": sorted(actual.keys() - reference.keys()),
            }
        )
    forward: dict[int | float, int | float] = {}
    reverse: dict[int | float, int | float] = {}
    for vertex in reference.keys() & actual.keys():
        reference_label = reference[vertex]
        actual_label = actual[vertex]
        if (
            reference_label in forward
            and forward[reference_label] != actual_label
        ) or (actual_label in reverse and reverse[actual_label] != reference_label):
            failures.append(
                {
                    "vertex_id": vertex,
                    "reference": reference_label,
                    "actual": actual_label,
                    "reason": "non_bijective_partition_labels",
                }
            )
            continue
        forward[reference_label] = actual_label
        reverse[actual_label] = reference_label
    return {
        "passed": not failures,
        "method": "equivalence",
        "failure_count": len(failures),
        "failures": failures[:10],
    }


def summarize(values: list[float]) -> dict[str, float]:
    return {
        "minimum": min(values),
        "median": statistics.median(values),
        "maximum": max(values),
        "mean": statistics.fmean(values),
    }


def run_case(
    case: TestCase,
    cli: Path,
    extension: Path,
    cache_dir: Path,
    threads: int,
    warmups: int,
    runs: int,
) -> dict[str, Any]:
    archive, archive_sha256 = download(case, cache_dir)
    with tempfile.TemporaryDirectory(prefix=f"duckgql-{case.name}-") as temporary:
        directory = Path(temporary)
        extracted = extract_archive(archive, directory / "official")
        nodes_csv, edges_csv, vertices, logical_edges, stored_arcs = write_graph_inputs(
            case, extracted, directory
        )
        timers, mapping_csv, result_csv = run_cli(
            cli,
            extension,
            case,
            nodes_csv,
            edges_csv,
            directory,
            threads,
            warmups,
            runs,
        )
        floating = case.algorithm in {"PR", "LCC"}
        reference = read_reference(
            extracted / f"{case.name}-{case.algorithm}", floating=floating
        )
        actual = read_actual(result_csv, read_mapping(mapping_csv), floating=floating)
        if case.algorithm == "BFS":
            actual = {vertex: actual.get(vertex, INFINITY) for vertex in vertices}
            validation = exact_validation(reference, actual)
        elif case.algorithm == "WCC":
            validation = equivalence_validation(reference, actual)
        else:
            validation = epsilon_validation(reference, actual)
        measured = [timers[f"run_{index}"] for index in range(runs)]
        return {
            "dataset": case.name,
            "algorithm": case.algorithm,
            "directed": case.directed,
            "vertices": len(vertices),
            "logical_edges": logical_edges,
            "stored_csr_arcs": stored_arcs,
            "archive": {
                "url": f"{BASE_URL}/{archive.name}",
                "sha256": archive_sha256,
            },
            "parameters": {
                key: value
                for key, value in {
                    "source_vertex": case.source,
                    "damping_factor": case.damping,
                    "num_iterations": case.iterations,
                }.items()
                if value is not None
            },
            "timing_seconds": {
                "copy_graph": timers["copy_graph"],
                "csr_build": timers["csr_build"],
                "warm_algorithm": summarize(measured),
            },
            "validation": validation,
        }


def markdown_report(result: dict[str, Any]) -> str:
    lines = [
        "# LDBC Graphalytics test-dataset validation",
        "",
        f"Generated: {result['generated_at']}",
        "",
        (
            "This is a local compatibility run over the official Graphalytics test "
            "archives and bundled reference outputs. It is not an audited or published "
            "Graphalytics benchmark submission."
        ),
        "",
        f"- CLI: `{result['environment']['cli']}`",
        f"- Extension: `{result['environment']['extension']}`",
        f"- Threads: {result['configuration']['threads']}",
        "- Warmups / measured runs: "
        f"{result['configuration']['warmups']} / {result['configuration']['runs']}",
        "- Passing cases: "
        f"{result['summary']['passed_cases']} / {result['summary']['total_cases']}",
        "",
        "| Dataset | Kernel | Shape | Validation | COPY GRAPH | CSR build | Warm median |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for case in result["cases"]:
        timing = case["timing_seconds"]
        validation = "PASS" if case["validation"]["passed"] else "FAIL"
        lines.append(
            "| {dataset} | {algorithm} | {vertices} V / {edges} E | {validation} | "
            "{copy:.6f}s | {csr:.6f}s | {warm:.6f}s |".format(
                dataset=case["dataset"],
                algorithm=case["algorithm"],
                vertices=case["vertices"],
                edges=case["logical_edges"],
                validation=validation,
                copy=timing["copy_graph"],
                csr=timing["csr_build"],
                warm=timing["warm_algorithm"]["median"],
            )
        )
    lines.extend(
        [
            "",
            "## Contract coverage",
            "",
            "- BFS: exact match; absent traversal rows are converted to Graphalytics infinity.",
            "- PageRank and LCC: Graphalytics relative epsilon validation (`0.0001`).",
            "- WCC: partition-equivalence validation with a bijection between component labels.",
            "- Undirected datasets: each logical input edge is expanded to two directed CSR arcs.",
            "- CDLP: not run because DuckGQL does not implement this kernel.",
            "- SSSP: not run because DuckGQL currently implements unweighted "
            "hop distance, while Graphalytics requires double edge weights.",
            "",
            "## Failures",
            "",
        ]
    )
    failures = [case for case in result["cases"] if not case["validation"]["passed"]]
    if not failures:
        lines.append("None.")
    else:
        for case in failures:
            lines.append(f"### {case['dataset']}")
            lines.append("")
            lines.append("```json")
            lines.append(json.dumps(case["validation"], indent=2, sort_keys=True))
            lines.append("```")
            lines.append("")
    lines.extend(
        [
            "## Reproduce",
            "",
            "```sh",
            result["reproduce"],
            "```",
            "",
            f"Specification: {SPEC_URL}",
            "",
        ]
    )
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
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
        "--output", type=Path, default=Path("build/benchmarks/graphalytics-test-current.json")
    )
    parser.add_argument(
        "--report", type=Path, default=Path("docs/benchmarks/graphalytics-test.md")
    )
    parser.add_argument("--threads", type=int, default=min(os.cpu_count() or 1, 8))
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--runs", type=int, default=7)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    cli = args.cli.resolve()
    extension = args.extension.resolve()
    if not cli.is_file():
        raise SystemExit(f"CLI does not exist: {cli}")
    if not extension.is_file():
        raise SystemExit(f"extension does not exist: {extension}")
    if args.threads <= 0 or args.warmups < 0 or args.runs <= 0:
        raise SystemExit("threads and runs must be positive; warmups must be non-negative")
    cases = []
    for case in TEST_CASES:
        print(f"[{case.name}] downloading, running, and validating", flush=True)
        result = run_case(
            case,
            cli,
            extension,
            args.cache_dir.resolve(),
            args.threads,
            args.warmups,
            args.runs,
        )
        status = "PASS" if result["validation"]["passed"] else "FAIL"
        print(
            f"[{case.name}] {status}; warm median "
            f"{result['timing_seconds']['warm_algorithm']['median']:.6f}s",
            flush=True,
        )
        cases.append(result)
    passed = sum(case["validation"]["passed"] for case in cases)
    command = (
        "python3 scripts/benchmark/benchmark_graphalytics.py "
        f"--threads {args.threads} --warmups {args.warmups} --runs {args.runs}"
    )
    result: dict[str, Any] = {
        "schema_version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "benchmark": "LDBC Graphalytics official test datasets",
        "qualification": "local compatibility run; not an audited submission",
        "specification": SPEC_URL,
        "environment": {
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cli": str(cli),
            "extension": str(extension),
        },
        "configuration": {
            "threads": args.threads,
            "warmups": args.warmups,
            "runs": args.runs,
        },
        "summary": {
            "total_cases": len(cases),
            "passed_cases": passed,
            "failed_cases": len(cases) - passed,
            "implemented_official_kernels": ["BFS", "PR", "WCC", "LCC"],
            "unsupported_official_kernels": ["CDLP", "weighted SSSP"],
        },
        "cases": cases,
        "reproduce": command,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(markdown_report(result))
    print(f"wrote {args.output}")
    print(f"wrote {args.report}")
    return 0 if passed == len(cases) else 1


if __name__ == "__main__":
    sys.exit(main())
