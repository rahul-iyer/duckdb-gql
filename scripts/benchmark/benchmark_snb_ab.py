#!/usr/bin/env python3
"""Profile/validate SNB reads using one CSR build per variant and round.

Uses the same query recipes and parameters as benchmark_snb_gql_interactive.py.
Does not import graph data or create property indexes. DuckGQL may initialize
its catalog tables when opening the database. A/B result validation
compares exact ordered CSV rows against current relational query results.
"""

import argparse
import csv
import hashlib
import json
import platform
import re
import statistics
import subprocess
import time
from pathlib import Path

from benchmark_snb_gql_interactive import build_benchmark_cases, sql_literal

from duckdb_profile import normalize_profile


def run(cli, database, sql, timeout, readonly=False):
    result = subprocess.run(
        [str(cli), "-unsigned", "-no-init", "-bail", "-csv", "-noheader",
         *(["-readonly"] if readonly else []), str(database)],
        input=sql, capture_output=True, text=True, timeout=timeout,
    )
    if result.returncode:
        raise RuntimeError(f"{cli}: {result.stderr[:2000]}\n{result.stderr[-3000:]}\n{result.stdout[-1000:]}")
    return result


def rows(path):
    with path.open() as source:
        return list(csv.reader(source))


def setting_statements(settings):
    statements = []
    for setting in settings:
        name, separator, value = setting.partition("=")
        if not separator or not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name):
            raise ValueError(f"expected setting_name=value, got {setting!r}")
        statements.append(f"SET {name}={sql_literal(value)};\n")
    return "".join(statements)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--source-database", type=Path, default=Path("build/benchmarks/snb10/snb10-relational.duckdb"))
    parser.add_argument("--graph-database", type=Path, default=Path("build/benchmarks/snb10/snb10-gql.duckdb"))
    parser.add_argument("--baseline-graph-database", type=Path)
    parser.add_argument("--candidate-graph-database", type=Path)
    parser.add_argument("--baseline-setting", action="append", default=[])
    parser.add_argument("--candidate-setting", action="append", default=[])
    parser.add_argument("--mode", choices=("gql", "sql"), default="gql")
    parser.add_argument("--parameters", type=Path, default=Path("build/benchmarks/snb10/interactive-results-4t-8gb.json"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--memory-limit", default="8GB")
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=300)
    parser.add_argument("--query", action="append")
    args = parser.parse_args()
    if min(args.rounds, args.runs, args.threads) < 1:
        parser.error("rounds, runs, and threads must be positive")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    cases, unsupported = build_benchmark_cases(args.parameters, set(args.query) if args.query else None)
    parameter_document = json.loads(args.parameters.read_text())
    prefix = f"SET threads={args.threads}; SET memory_limit={sql_literal(args.memory_limit)}; SET preserve_insertion_order=false;\n"
    reference = prefix
    for name, case in cases.items():
        reference += f".once {sql_literal(out / (name + '-expected.csv'))}\n{case['sql']}\n"
    (out / "reference.sql").write_text(reference)
    run(args.baseline.resolve(), args.source_database.resolve(), reference, args.timeout, readonly=True)
    print(f"Relational reference: {len(cases)} queries complete", flush=True)
    variants = {"baseline": args.baseline.resolve()}
    if args.candidate:
        variants["candidate"] = args.candidate.resolve()
    graph_databases = {
        "baseline": (args.baseline_graph_database or args.graph_database).resolve(),
        "candidate": (args.candidate_graph_database or args.graph_database).resolve(),
    }
    settings = {"baseline": args.baseline_setting, "candidate": args.candidate_setting}
    samples = {name: {variant: [] for variant in variants} for name in cases}
    measurements = {name: {variant: [] for variant in variants} for name in cases}
    processes = []
    for round_id in range(args.rounds):
        order = list(variants) if round_id % 2 == 0 else list(reversed(variants))
        for variant in order:
            directory = out / f"{variant}-{round_id}"
            directory.mkdir(exist_ok=True)
            script = ".output /dev/null\n" + prefix
            script += setting_statements(settings[variant])
            script += f"SET temp_directory={sql_literal(directory / 'spill')};\n"
            if args.mode == "gql":
                script += f"PRAGMA enable_profiling='json'; PRAGMA profiling_output={sql_literal(directory / 'csr-build.json')};\n"
                script += "CALL gql_build_csr('snb_interactive');\nPRAGMA disable_profiling;\nSESSION SET GRAPH snb_interactive;\n"
            for name, case in cases.items():
                query = case[args.mode]
                script += query + "\n"  # warmup
                for sample in range(args.runs):
                    stem = directory / f"{name}-{sample}"
                    script += f"PRAGMA enable_profiling='json'; PRAGMA profiling_output={sql_literal(str(stem) + '.json')};\n"
                    script += f".once {sql_literal(str(stem) + '.csv')}\n{query}\nPRAGMA disable_profiling;\n"
            if args.mode == "gql":
                script += f".once {sql_literal(directory / 'csr.csv')}\nSELECT * FROM gql_csr_stats('snb_interactive');\n"
            (directory / "run.sql").write_text(script)
            print(f"Round {round_id + 1}: {variant} executing {args.mode.upper()} queries", flush=True)
            database = graph_databases[variant] if args.mode == "gql" else args.source_database.resolve()
            started = time.monotonic()
            result = run(variants[variant], database, script, args.timeout, readonly=args.mode == "sql")
            process = {"round": round_id, "variant": variant, "wall_seconds": time.monotonic() - started}
            (directory / "stdout.log").write_text(result.stdout)
            (directory / "stderr.log").write_text(result.stderr)
            if args.mode == "gql":
                csr_rows = rows(directory / "csr.csv")
                if len(csr_rows) != 1 or csr_rows[0][5] != "1" or csr_rows[0][6] != "true":
                    raise AssertionError(f"{variant}/{round_id}: expected one cached CSR build, got {csr_rows}")
                csr = csr_rows[0]
                process["csr"] = {"vertex_count": int(csr[2]), "edge_count": int(csr[3]),
                                  "memory_bytes": int(csr[4]), "build_count": int(csr[5]),
                                  "profile_seconds": normalize_profile(json.loads((directory / "csr-build.json").read_text()))["latency"]}
            processes.append(process)
            for name in cases:
                expected = rows(out / (name + "-expected.csv"))
                for sample in range(args.runs):
                    stem = directory / f"{name}-{sample}"
                    actual = rows(Path(str(stem) + ".csv"))
                    if actual != expected:
                        raise AssertionError(f"{variant}/{name}/{sample}: ordered CSV mismatch")
                    profile = normalize_profile(json.loads(Path(str(stem) + ".json").read_text()))
                    samples[name][variant].append(profile["latency"])
                    measurements[name][variant].append({
                        "round": round_id, "sample": sample, "latency_seconds": profile["latency"],
                        "cpu_seconds": profile["cpu_time"], "rows_scanned": profile["cumulative_rows_scanned"],
                        "intermediate_rows": profile["cumulative_cardinality"],
                    })
                median = statistics.median(samples[name][variant])
                print(f"  {name}: verified, {median * 1000:.3f} ms median", flush=True)
    report = {
        "platform": platform.platform(), "threads": args.threads, "memory_limit": args.memory_limit,
        "rounds": args.rounds, "runs_per_round": args.runs,
        "source_database": str(args.source_database.resolve()),
        "graph_databases": {variant: str(graph_databases[variant]) for variant in variants},
        "mode": args.mode,
        "settings": settings,
        "processes": processes,
        "parameters": {name: parameter_document["queries"][name]["parameters"] for name in cases},
        "scope": "Warm SF10 SNB reads; GQL builds CSR once per variant/round outside query measurements; exact ordered CSV vs live relational reference",
        "executables": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                        for name, path in variants.items()},
        "queries": {name: {"seconds": values,
                            "medians": {variant: statistics.median(times) for variant, times in values.items()},
                            "gql": cases[name]["gql"].strip(),
                            "sql": cases[name]["sql"].strip(),
                            "result_rows": len(rows(out / (name + "-expected.csv"))),
                            "measurements": measurements[name],
                            "result_sha256": hashlib.sha256((out / (name + "-expected.csv")).read_bytes()).hexdigest()}
                    for name, values in samples.items()},
        "unsupported": unsupported,
    }
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
