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
import statistics
import subprocess
from pathlib import Path

from benchmark_snb_gql_interactive import build_benchmark_cases, sql_literal

from duckdb_profile import normalize_profile


def run(cli, database, sql, timeout):
    result = subprocess.run(
        [str(cli), "-unsigned", "-no-init", "-bail", "-csv", "-noheader", str(database)],
        input=sql, capture_output=True, text=True, timeout=timeout,
    )
    if result.returncode:
        raise RuntimeError(f"{cli}: {result.stderr[:2000]}\n{result.stderr[-3000:]}\n{result.stdout[-1000:]}")


def rows(path):
    with path.open() as source:
        return list(csv.reader(source))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--source-database", type=Path, default=Path("build/benchmarks/snb10/snb10-relational.duckdb"))
    parser.add_argument("--graph-database", type=Path, default=Path("build/benchmarks/snb10/snb10-gql.duckdb"))
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
    run(args.baseline.resolve(), args.source_database.resolve(), reference, args.timeout)
    print(f"Relational reference: {len(cases)} queries complete", flush=True)
    variants = {"baseline": args.baseline.resolve()}
    if args.candidate:
        variants["candidate"] = args.candidate.resolve()
    samples = {name: {variant: [] for variant in variants} for name in cases}
    for round_id in range(args.rounds):
        order = list(variants) if round_id % 2 == 0 else list(reversed(variants))
        for variant in order:
            directory = out / f"{variant}-{round_id}"
            directory.mkdir(exist_ok=True)
            script = ".output /dev/null\n" + prefix
            script += f"SET temp_directory={sql_literal(directory / 'spill')};\n"
            script += "CALL gql_build_csr('snb_interactive');\nSESSION SET GRAPH snb_interactive;\n"
            for name, case in cases.items():
                query = case["gql"]
                script += query + "\n"  # warmup
                for sample in range(args.runs):
                    stem = directory / f"{name}-{sample}"
                    script += f"PRAGMA enable_profiling='json'; PRAGMA profiling_output={sql_literal(str(stem) + '.json')};\n"
                    script += f".once {sql_literal(str(stem) + '.csv')}\n{query}\nPRAGMA disable_profiling;\n"
            script += f".once {sql_literal(directory / 'csr.csv')}\nSELECT * FROM gql_csr_stats('snb_interactive');\n"
            (directory / "run.sql").write_text(script)
            print(f"Round {round_id + 1}: {variant} building CSR and executing queries", flush=True)
            run(variants[variant], args.graph_database.resolve(), script, args.timeout)
            for name in cases:
                expected = rows(out / (name + "-expected.csv"))
                for sample in range(args.runs):
                    stem = directory / f"{name}-{sample}"
                    actual = rows(Path(str(stem) + ".csv"))
                    if actual != expected:
                        raise AssertionError(f"{variant}/{name}/{sample}: ordered CSV mismatch")
                    profile = normalize_profile(json.loads(Path(str(stem) + ".json").read_text()))
                    samples[name][variant].append(profile["latency"])
                median = statistics.median(samples[name][variant])
                print(f"  {name}: verified, {median * 1000:.3f} ms median", flush=True)
    report = {
        "platform": platform.platform(), "threads": args.threads, "memory_limit": args.memory_limit,
        "rounds": args.rounds, "runs_per_round": args.runs,
        "source_database": str(args.source_database.resolve()),
        "graph_database": str(args.graph_database.resolve()),
        "parameters": {name: parameter_document["queries"][name]["parameters"] for name in cases},
        "scope": "Warm managed SF10 SNB reads; one CSR build per variant/round; exact ordered CSV vs live relational reference",
        "executables": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                        for name, path in variants.items()},
        "queries": {name: {"seconds": values,
                            "medians": {variant: statistics.median(times) for variant, times in values.items()},
                            "gql": cases[name]["gql"].strip(),
                            "result_sha256": hashlib.sha256((out / (name + "-expected.csv")).read_bytes()).hexdigest()}
                    for name, values in samples.items()},
        "unsupported": unsupported,
    }
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
