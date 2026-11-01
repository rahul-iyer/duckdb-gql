#!/usr/bin/env python3
"""Check CSR trail results against Python, then run interleaved CLI A/B profiles.

Each executable must contain the corresponding DuckGQL build. Setup/CSR build
and warmup are excluded from measured query latency. This is a focused managed
graph microbenchmark, not an LDBC result or a cold/referenced-storage benchmark.
"""

import argparse
import csv
import hashlib
import json
import platform
import statistics
import subprocess
from pathlib import Path

from duckdb_profile import normalize_profile


def literal(value):
    return "'" + str(value).replace("'", "''") + "'"


def run(cli, sql, timeout=120):
    result = subprocess.run(
        [str(cli), "-unsigned", "-no-init", "-bail", "-csv", "-noheader"],
        input=sql, capture_output=True, text=True, timeout=timeout,
    )
    if result.returncode:
        raise RuntimeError(f"{cli}: {result.stderr}\n{result.stdout}")
    return result.stdout


def fixture(directory, name, vertices, edges):
    nodes_path, edges_path = directory / f"{name}_nodes.csv", directory / f"{name}_edges.csv"
    with nodes_path.open("w") as out:
        writer = csv.writer(out)
        writer.writerow([":ID", ":LABEL", "id:int"])
        writer.writerows((v, "Node", v) for v in range(1, vertices + 1))
    with edges_path.open("w") as out:
        writer = csv.writer(out)
        writer.writerow([":START_ID", ":END_ID", ":TYPE"])
        writer.writerows(edges)
    return f"""
CREATE GRAPH {name} ANY;
COPY GRAPH {name} FROM (VERTICES {literal(nodes_path)}, EDGES {literal(edges_path)}) FORMAT GRAPH;
CALL gql_build_csr('{name}');
"""


def expansion(graph, seeds, minimum, maximum, direction="out", unbounded=False, label="route"):
    values = ",".join(f"({s}::UBIGINT)" if s is not None else "(NULL::UBIGINT)" for s in seeds)
    return f"""FROM (VALUES {values}) seed(id),
LATERAL gql_csr_path_expand(struct_pack(
    graph_name := '{graph}', vertex_id := seed.id, direction := '{direction}', edge_label := '{label}',
    minimum_repetitions := {minimum}::UBIGINT, maximum_repetitions := {maximum}::UBIGINT,
    unbounded := {str(unbounded).lower()}
)) p"""


def oracle(edges, seeds, minimum, maximum, direction="out", unbounded=False, label="route"):
    adjacency = {}
    for edge_id, (source, target, kind) in enumerate(edges, 1):
        if kind != label:
            continue
        if direction == "in":
            source, target = target, source
        adjacency.setdefault(source, []).append((edge_id, target))
    result = []
    for seed in seeds:
        if seed is None:
            continue
        if minimum == 0:
            result.append((0, seed, seed, label))
        # Independent iterative enumerator; immutable sets isolate sibling paths.
        pending = [(seed, frozenset())]
        while pending:
            vertex, used = pending.pop()
            for edge_id, neighbor in adjacency.get(vertex, []):
                if edge_id in used:
                    continue
                depth = len(used) + 1
                if depth >= minimum:
                    endpoints = (seed, neighbor) if direction == "out" else (neighbor, seed)
                    result.append((edge_id, *endpoints, label))
                if unbounded or depth < maximum:
                    pending.append((neighbor, used | {edge_id}))
    return sorted(result)


def validate(cli, setup, edges):
    cases = [
        ([1, 1, 65, None], 1, 4, "out", False),
        ([1, 65], 63, 68, "out", False),
        ([1, 65], 1, 0, "out", True),
        ([1, 70], 1, 0, "in", True),
        ([1, 71], 0, 0, "out", True),
    ]
    for seeds, minimum, maximum, direction, unbounded in cases:
        query = expansion("check_paths", seeds, minimum, maximum, direction, unbounded)
        output = run(cli, ".output /dev/null\n" + setup + ".output stdout\n"
                     + "SELECT p.* " + query + ";\n")
        actual = sorted((int(a), int(b), int(c), d) for a, b, c, d in csv.reader(output.splitlines()))
        expected = oracle(edges, seeds, minimum, maximum, direction, unbounded)
        if actual != expected:
            raise AssertionError(f"{cli}: mismatch for {seeds, minimum, maximum, direction, unbounded}: "
                                 f"{len(actual)} actual versus {len(expected)} expected")
    return len(cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=4)
    parser.add_argument("--threads", type=int, default=1)
    args = parser.parse_args()
    if args.rounds < 2:
        parser.error("at least two rounds are required for interleaved A/B ordering")
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    prefix = f"LOAD duckgql; SET threads={args.threads}; SET preserve_insertion_order=true;\n"
    check_edges = [(i, i + 1, "route") for i in range(1, 70)]
    check_edges += [(70, 65, "route"), (65, 67, "route"), (65, 67, "route"),
                    (66, 66, "route"), (1, 70, "other")]
    check_setup = prefix + fixture(directory, "check_paths", 71, check_edges)
    checks = {name: validate(cli.resolve(), check_setup, check_edges)
              for name, cli in (("baseline", args.baseline), ("candidate", args.candidate))}

    setup = prefix
    setup += fixture(directory, "chain", 16385, [(i, i + 1, "route") for i in range(1, 16385)])
    setup += fixture(directory, "hub", 65537, [(1, i, "route") for i in range(2, 65538)])
    setup += fixture(directory, "cycle", 4096,
                     [(i, i % 4096 + 1, "route") for i in range(1, 4097)])
    setup += fixture(directory, "dense", 12,
                     [(i, j, "route") for i in range(1, 13) for j in range(1, 13) if i != j])
    setup += fixture(directory, "mixed", 4097,
                     [(i, i + 1, "route") for i in range(1, 4097)]
                     + [(i, (i + k) % 4097 + 1, "other") for i in range(1, 4098) for k in range(8)])
    specs = [
        ("chain_short", "chain", list(range(1, 8193)), 1, 4, "out", False),
        ("chain_medium", "chain", list(range(1, 257)), 1, 128, "out", False),
        ("chain_long", "chain", [1], 1, 16384, "out", False),
        ("chain_reverse", "chain", [16385], 1, 16384, "in", False),
        ("hub", "hub", [1], 1, 2, "out", False),
        ("cycle_unbounded", "cycle", [1, 1000], 1, 0, "out", True),
        ("dense_short", "dense", [1], 1, 5, "out", False),
        ("mixed_labels", "mixed", [1], 1, 4096, "out", False),
    ]
    queries = {name: "SELECT count(*), sum(__gql_edge_id), sum(__gql_source_id), sum(__gql_target_id) "
               + expansion(graph, seeds, lo, hi, direction, unbounded) + ";"
               for name, graph, seeds, lo, hi, direction, unbounded in specs}
    # Exercise actual planner-selected GQL too, including endpoint filtering.
    queries["gql_chain_long"] = """SESSION SET GRAPH chain;
MATCH (source:Node) WHERE source.id = 1
MATCH (source)-[:route]->+(target:Node)
RETURN count(*);"""
    samples = {name: {variant: [] for variant in checks} for name in queries}
    results = {}
    for round_id in range(args.rounds):
        order = ("baseline", "candidate") if round_id % 2 == 0 else ("candidate", "baseline")
        for variant in order:
            cli = getattr(args, variant).resolve()
            script = ".output /dev/null\n" + setup
            for name, query in queries.items():
                profile = directory / f"{variant}_{round_id}_{name}.json"
                result = directory / f"{variant}_{round_id}_{name}.csv"
                script += query + "\n"  # warmup
                script += f"PRAGMA enable_profiling='json'; PRAGMA profiling_output={literal(profile)};\n"
                script += f".output {result}\n{query}\n.output /dev/null\nPRAGMA disable_profiling;\n"
            run(cli, script)
            for name in queries:
                profile = normalize_profile(json.loads((directory / f"{variant}_{round_id}_{name}.json").read_text()))
                samples[name][variant].append(profile["latency"])
                result = (directory / f"{variant}_{round_id}_{name}.csv").read_text()
                if name in results and results[name] != result:
                    raise AssertionError(f"A/B result mismatch: {variant}/{name}")
                results[name] = result
            print(f"round {round_id + 1}/{args.rounds}: {variant} complete", flush=True)
    report = {
        "platform": platform.platform(), "threads": args.threads,
        "oracle_cases_passed": checks, "rounds": args.rounds,
        "scope": "warm managed CSR; profile latency excludes graph/CSR setup and warmup",
        "executables": {name: {"path": str(getattr(args, name).resolve()),
                               "sha256": hashlib.sha256(getattr(args, name).read_bytes()).hexdigest()}
                        for name in checks},
        "cases": {name: {"seconds": values,
                          "speedup": statistics.median(values["baseline"]) / statistics.median(values["candidate"]),
                          "result": results[name].strip()}
                  for name, values in samples.items()},
    }
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    for name, result in report["cases"].items():
        print(f"{name}: {result['speedup']:.3f}x")


if __name__ == "__main__":
    main()
