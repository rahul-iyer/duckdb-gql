#!/usr/bin/env python3
"""Compare graph optimizer latency against fixed expected rows on a skewed graph."""

import argparse
import csv
import json
import statistics
import subprocess
import tempfile
from pathlib import Path

from duckdb_profile import normalize_profile


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--leaves", type=int, default=10000)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    if min(args.leaves, args.runs, args.threads) < 1 or args.warmups < 0 or args.timeout <= 0:
        parser.error("leaves, runs, threads and timeout must be positive; warmups must be nonnegative")
    cases = {
        "parallel_edges": (
            "MATCH (a:Node)-[:R]->(b:Node) WHERE a.id=1 RETURN b.id,b.score ORDER BY b.id;",
            [["2", "20"], ["2", "20"], ["3", "NULL"]],
        ),
        "optional_null": (
            "MATCH (a:Node) WHERE a.id=4 OPTIONAL MATCH (a)-[:R]->(b:Node) " "WHERE b.score>25 RETURN a.id,b.score;",
            [["4", "NULL"]],
        ),
        "nullable_aggregate": (
            "MATCH (a:Node)-[e:R]->(b:Node) WHERE a.id=1 RETURN count(e),count(b.score),sum(b.score);",
            [["3", "2", "40"]],
        ),
        "hub": ("MATCH (:Hub)-[:R]->(:Leaf) RETURN count(*);", [[str(args.leaves)]]),
    }
    report = {
        "settings": vars(args) | {"duckdb": str(args.duckdb.resolve()), "output": str(args.output)},
        "results": {},
    }
    with tempfile.TemporaryDirectory(prefix="duckgql-optimizer-") as directory:
        root = Path(directory)
        nodes, edges = root / "nodes.csv", root / "edges.csv"
        with nodes.open("w", newline="") as target:
            writer = csv.writer(target)
            writer.writerow([":ID", "id:int", "score:int", ":LABEL"])
            writer.writerows(
                [(1, 1, "", "Node"), (2, 2, 20, "Node"), (3, 3, "", "Node"), (4, 4, "", "Node"), (10, 10, 10, "Hub")]
            )
            writer.writerows((i, i, i, "Leaf") for i in range(11, 11 + args.leaves))
        with edges.open("w", newline="") as target:
            writer = csv.writer(target)
            writer.writerow([":START_ID", ":END_ID", ":TYPE"])
            writer.writerows([(1, 2, "R"), (1, 2, "R"), (1, 3, "R")])
            writer.writerows((10, i, "R") for i in range(11, 11 + args.leaves))
        for csr in (False, True):
            for mode in ("legacy", "cost"):
                stem = f"{mode}-{'csr' if csr else 'tables'}"
                script = [
                    ".output /dev/null",
                    ".nullvalue NULL",
                    f"SET threads={args.threads};",
                    "SET memory_limit='1GB';",
                    f"SET gql_optimizer='{mode}';",
                    "CREATE GRAPH comparison ANY;",
                    f"COPY GRAPH comparison FROM (VERTICES {literal(nodes)}, EDGES {literal(edges)}) FORMAT GRAPH;",
                    "SESSION SET GRAPH comparison;",
                ]
                if csr:
                    script.append("CALL gql_build_csr('comparison');")
                for name, (query, _) in cases.items():
                    script.extend([query] * args.warmups)
                    for sample in range(args.runs):
                        sample_path = root / f"{stem}-{name}-{sample}"
                        script.extend(
                            [
                                "PRAGMA enable_profiling='json';",
                                f"PRAGMA profiling_output={literal(str(sample_path) + '.json')};",
                                f".once {literal(str(sample_path) + '.csv')}",
                                query,
                                "PRAGMA disable_profiling;",
                            ]
                        )
                result = subprocess.run(
                    [str(args.duckdb.resolve()), "-unsigned", "-no-init", "-bail", "-csv", "-noheader", ":memory:"],
                    input="\n".join(script) + "\n",
                    capture_output=True,
                    text=True,
                    timeout=args.timeout,
                )
                if result.returncode:
                    raise RuntimeError(f"{stem}: {result.stderr}\n{result.stdout}")
                report["results"][stem] = {}
                for name, (_, expected) in cases.items():
                    timings = []
                    for sample in range(args.runs):
                        sample_path = root / f"{stem}-{name}-{sample}"
                        with Path(str(sample_path) + ".csv").open() as source:
                            actual = list(csv.reader(source))
                        if actual != expected:
                            raise AssertionError(f"{stem}/{name}/{sample}: {actual!r} != {expected!r}")
                        profile = normalize_profile(json.loads(Path(str(sample_path) + ".json").read_text()))
                        timings.append(profile["latency"] * 1000)
                    report["results"][stem][name] = {
                        "median_ms": statistics.median(timings),
                        "samples_ms": timings,
                        "rows_verified": len(expected),
                    }
        for access in ("tables", "csr"):
            report["results"][f"cost_vs_legacy_{access}"] = {
                name: report["results"][f"cost-{access}"][name]["median_ms"]
                / report["results"][f"legacy-{access}"][name]["median_ms"]
                for name in cases
            }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Verified {len(cases) * 4 * args.runs} result sets; report: {args.output}")


if __name__ == "__main__":
    main()
