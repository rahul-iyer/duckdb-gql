#!/usr/bin/env python3
"""Generate Graph500 data and benchmark DuckGQL PageRank by scale.

The DuckDB memory_limit setting is applied to import/query work. DuckGQL's
connection-local CSR and PageRank vectors are native extension allocations and
are therefore guarded separately with a physical-memory estimate.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

GRAPH500_URL = "https://github.com/graph500/graph500.git"
EDGE_FACTOR = 16
PAGERANK_BYTES_PER_VERTEX = 24
CSR_OFFSET_BYTES = 8
COMPACT_NEIGHBOR_BYTES = 4
WIDE_NEIGHBOR_BYTES = 8

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[1]
GENERATOR_SOURCE = SCRIPT_DIR / "generate_graph500_csv.c"
PAGERANK_SQL_TEMPLATE = SCRIPT_DIR / "pagerank_loaded.sql.in"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run DuckGQL PageRank over progressively larger official Graph500 edge tuples."
    )
    parser.add_argument(
        "--scales",
        type=int,
        nargs="+",
        default=[20],
        help="Graph500 SCALE values to run in order (default: 20).",
    )
    parser.add_argument(
        "--memory-limit",
        default="4GB",
        help="DuckDB buffer-manager memory_limit (default: 4GB).",
    )
    parser.add_argument(
        "--work-dir",
        type=Path,
        default=Path(tempfile.gettempdir()) / "duckgql-graph500",
        help="Generated data, databases, logs, and results directory.",
    )
    parser.add_argument(
        "--graph500-dir",
        type=Path,
        help="Existing Graph500 checkout; cloned under WORK_DIR when omitted.",
    )
    parser.add_argument(
        "--duckdb",
        type=Path,
        default=REPO_ROOT / "build/release/duckdb",
        help="DuckDB CLI built for this repository.",
    )
    parser.add_argument(
        "--extension",
        type=Path,
        default=REPO_ROOT / "build/release/extension/duckgql/duckgql.duckdb_extension",
        help="DuckGQL extension artifact.",
    )
    parser.add_argument("--max-iterations", type=int, default=100)
    parser.add_argument("--tolerance", type=float, default=1e-8)
    parser.add_argument(
        "--max-native-memory-fraction",
        type=float,
        default=0.75,
        help="Refuse estimated native allocations above this fraction of physical RAM (default: 0.75).",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Run even when the native-memory estimate exceeds the safety threshold.",
    )
    parser.add_argument(
        "--regenerate",
        action="store_true",
        help="Regenerate CSV files even when complete-looking files already exist.",
    )
    parser.add_argument(
        "--reimport",
        action="store_true",
        help="Recreate the DuckDB graph even when the database already exists.",
    )
    parser.add_argument(
        "--delete-csv-after-import",
        action="store_true",
        help="Delete nodes.csv and edges.csv after a successful import.",
    )
    return parser.parse_args()


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    stdin: str | None = None,
    output_path: Path | None = None,
) -> tuple[str, float]:
    started = time.perf_counter()
    if output_path:
        with output_path.open("w", encoding="utf-8") as output:
            completed = subprocess.run(
                command,
                cwd=cwd,
                input=stdin,
                text=True,
                stdout=output,
                stderr=subprocess.STDOUT,
                check=False,
            )
        text = output_path.read_text(encoding="utf-8", errors="replace")
    else:
        completed = subprocess.run(
            command,
            cwd=cwd,
            input=stdin,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        text = completed.stdout
    elapsed = time.perf_counter() - started
    if completed.returncode != 0 or re.search(r"(?m)^(?:[A-Za-z ]+)?Error:", text):
        if output_path:
            print(text, file=sys.stderr)
        raise RuntimeError(f"command failed with exit code {completed.returncode}: {' '.join(command)}")
    return text, elapsed


def physical_memory_bytes() -> int | None:
    if platform.system() == "Darwin":
        try:
            output = subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True, stderr=subprocess.DEVNULL)
            return int(output.strip())
        except (OSError, subprocess.SubprocessError, ValueError):
            try:
                output = subprocess.check_output(["memory_pressure", "-Q"], text=True, stderr=subprocess.DEVNULL)
                match = re.search(r"The system has (\d+)", output)
                return int(match.group(1)) if match else None
            except (OSError, subprocess.SubprocessError, ValueError):
                return None
    try:
        page_size = os.sysconf("SC_PAGE_SIZE")
        page_count = os.sysconf("SC_PHYS_PAGES")
        return int(page_size * page_count)
    except (OSError, ValueError):
        return None


def human_bytes(value: float) -> str:
    units = ["B", "KiB", "MiB", "GiB", "TiB"]
    unit = 0
    while value >= 1024 and unit < len(units) - 1:
        value /= 1024
        unit += 1
    return f"{value:.2f} {units[unit]}"


def estimate_native_bytes(scale: int) -> int:
    vertices = 1 << scale
    edges = EDGE_FACTOR * vertices
    neighbor_bytes = COMPACT_NEIGHBOR_BYTES if vertices - 1 <= (1 << 32) - 1 else WIDE_NEIGHBOR_BYTES
    # Unfiltered PageRank requests outgoing topology only. Dense generated IDs
    # are implicit, and edge identities, labels, incoming topology, and label
    # postings are not materialized.
    csr_bytes = CSR_OFFSET_BYTES * (vertices + 1) + neighbor_bytes * edges
    return csr_bytes + PAGERANK_BYTES_PER_VERTEX * vertices


def ensure_reference_checkout(path: Path) -> None:
    if (path / "generator/graph_generator.c").is_file():
        return
    if path.exists():
        raise RuntimeError(f"{path} exists but is not a Graph500 checkout")
    path.parent.mkdir(parents=True, exist_ok=True)
    print(f"Cloning the official Graph500 generator into {path}")
    run(["git", "clone", "--depth", "1", GRAPH500_URL, str(path)])


def compile_generator(reference: Path, binary: Path) -> None:
    binary.parent.mkdir(parents=True, exist_ok=True)
    sources = [
        GENERATOR_SOURCE,
        reference / "generator/graph_generator.c",
        reference / "generator/splittable_mrg.c",
        reference / "generator/utils.c",
    ]
    if binary.exists() and binary.stat().st_mtime >= max(path.stat().st_mtime for path in sources):
        return
    print("Compiling the streaming Graph500 CSV adapter")
    run(
        [
            os.environ.get("CC", "cc"),
            "-O3",
            f"-I{reference / 'generator'}",
            *(str(path) for path in sources),
            "-lm",
            "-o",
            str(binary),
        ]
    )


def sql_quote(value: Path | str) -> str:
    return str(value).replace("'", "''")


def looks_complete(path: Path, expected_last_id: int) -> bool:
    if not path.is_file() or path.stat().st_size == 0:
        return False
    with path.open("rb") as handle:
        handle.seek(max(0, path.stat().st_size - 256))
        last_line = handle.read().splitlines()[-1].decode("utf-8", errors="replace")
    return last_line.startswith(f"{expected_last_id},")


def generate_scale(generator: Path, scale: int, scale_dir: Path, regenerate: bool) -> tuple[Path, Path, float]:
    vertices = 1 << scale
    edges = EDGE_FACTOR * vertices
    nodes_csv = scale_dir / "nodes.csv"
    edges_csv = scale_dir / "edges.csv"
    if not regenerate and looks_complete(nodes_csv, vertices - 1) and looks_complete(edges_csv, edges - 1):
        print(f"Reusing complete CSV files for scale {scale}")
        return nodes_csv, edges_csv, 0.0
    scale_dir.mkdir(parents=True, exist_ok=True)
    print(f"Generating scale {scale}: {vertices:,} vertices, {edges:,} edge tuples " f"(streaming in 1M-edge chunks)")
    _, elapsed = run(
        [str(generator), str(scale), str(EDGE_FACTOR), str(nodes_csv), str(edges_csv)],
        output_path=scale_dir / "generate.log",
    )
    if not looks_complete(nodes_csv, vertices - 1) or not looks_complete(edges_csv, edges - 1):
        raise RuntimeError(f"generated CSV files for scale {scale} are incomplete")
    return nodes_csv, edges_csv, elapsed


def import_graph(
    duckdb: Path,
    extension: Path,
    database: Path,
    nodes_csv: Path,
    edges_csv: Path,
    memory_limit: str,
    reimport: bool,
) -> tuple[float, bool]:
    if database.is_file() and not reimport:
        print(f"Reusing imported graph at {database}")
        return 0.0, False
    sql = f"""
.timer on
LOAD '{sql_quote(extension)}';
SET memory_limit = '{memory_limit.replace("'", "''")}';
DROP GRAPH IF EXISTS graph500;
CREATE GRAPH graph500 ANY;
COPY GRAPH graph500 FROM (
    VERTICES '{sql_quote(nodes_csv)}',
    EDGES '{sql_quote(edges_csv)}'
) FORMAT GRAPH OPTIONS (VALIDATE FALSE);
"""
    print(f"Importing into {database} with DuckDB memory_limit={memory_limit}")
    _, elapsed = run(
        [str(duckdb), "-csv", str(database)],
        stdin=sql,
        output_path=database.parent / "import.log",
    )
    return elapsed, True


def phase_timings(output: str) -> dict[str, float]:
    current: str | None = None
    timings: dict[str, float] = {}
    for line in output.splitlines():
        if line.startswith("phase="):
            current = line.removeprefix("phase=")
            continue
        match = re.search(r"Run Time \(s\): real ([0-9.]+)", line)
        if current and match:
            timings[current] = float(match.group(1))
            current = None
    return timings


def parse_pagerank_result(output: str) -> dict[str, Any]:
    csr_match = re.search(r"(?m)^(\d+),(\d+),(\d+)$", output)
    rank_match = re.search(r"(?m)^(\d+),(true|false),([0-9.eE+-]+)$", output)
    if not csr_match or not rank_match:
        raise RuntimeError("could not parse CSR or PageRank result; inspect pagerank.log")
    return {
        "vertices": int(csr_match.group(1)),
        "edges": int(csr_match.group(2)),
        "csr_bytes": int(csr_match.group(3)),
        "iterations": int(rank_match.group(1)),
        "converged": rank_match.group(2) == "true",
        "rank_sum": float(rank_match.group(3)),
    }


def run_pagerank(
    duckdb: Path,
    extension: Path,
    database: Path,
    memory_limit: str,
    max_iterations: int,
    tolerance: float,
) -> tuple[dict[str, Any], dict[str, float], float]:
    sql = PAGERANK_SQL_TEMPLATE.read_text(encoding="utf-8")
    replacements = {
        "__DUCKGQL_EXTENSION__": sql_quote(extension),
        "__MEMORY_LIMIT__": memory_limit.replace("'", "''"),
        "max_iterations := 100": f"max_iterations := {max_iterations}",
        "tolerance := 1e-8": f"tolerance := {tolerance:.17g}",
    }
    for old, new in replacements.items():
        sql = sql.replace(old, new)
    print("Running cold PageRank with algorithm-owned CSR, then one warm PageRank")
    output, elapsed = run(
        [str(duckdb), "-csv", str(database)],
        stdin=sql,
        output_path=database.parent / "pagerank.log",
    )
    return parse_pagerank_result(output), phase_timings(output), elapsed


def write_results(path: Path, results: list[dict[str, Any]]) -> None:
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def main() -> int:
    args = parse_args()
    if not 0 < args.max_native_memory_fraction <= 1:
        raise ValueError("--max-native-memory-fraction must be in (0, 1]")
    if args.max_iterations <= 0 or args.tolerance <= 0:
        raise ValueError("PageRank max iterations and tolerance must be positive")
    for path, label in ((args.duckdb, "DuckDB CLI"), (args.extension, "DuckGQL extension")):
        if not path.is_file():
            raise FileNotFoundError(f"{label} not found: {path}")

    work_dir = args.work_dir.resolve()
    reference = (args.graph500_dir or work_dir / "graph500-reference").resolve()
    generator = work_dir / "bin/generate_graph500_csv"
    work_dir.mkdir(parents=True, exist_ok=True)
    ensure_reference_checkout(reference)
    compile_generator(reference, generator)

    physical = physical_memory_bytes()
    if physical:
        print(f"Physical memory: {human_bytes(physical)}")
    print(
        "Note: DuckDB memory_limit does not cap DuckGQL CSR/PageRank native allocations; "
        "the safety guard below does."
    )
    print(
        "Note: PageRank treats the official generator's tuple orientation as directed; "
        "this script does not symmetrize edges."
    )

    results_path = work_dir / "pagerank-results.json"
    results: list[dict[str, Any]] = []
    if results_path.is_file():
        try:
            results = json.loads(results_path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            results = []

    for scale in args.scales:
        if not 1 <= scale <= 40:
            raise ValueError(f"invalid scale {scale}; expected [1, 40]")
        estimated = estimate_native_bytes(scale)
        threshold = physical * args.max_native_memory_fraction if physical else None
        print(f"\nScale {scale}: estimated CSR + PageRank native memory " f"{human_bytes(estimated)}")
        if threshold and estimated > threshold and not args.force:
            print(
                f"Stopping before scale {scale}: estimate exceeds "
                f"{args.max_native_memory_fraction:.0%} of physical RAM "
                f"({human_bytes(threshold)}). Pass --force to override.",
                file=sys.stderr,
            )
            break

        scale_dir = work_dir / f"scale-{scale}"
        database = scale_dir / "benchmark.duckdb"
        if database.is_file() and not args.reimport:
            nodes_csv = scale_dir / "nodes.csv"
            edges_csv = scale_dir / "edges.csv"
            generation_seconds = 0.0
        else:
            nodes_csv, edges_csv, generation_seconds = generate_scale(generator, scale, scale_dir, args.regenerate)
        import_seconds, imported = import_graph(
            args.duckdb.resolve(),
            args.extension.resolve(),
            database,
            nodes_csv,
            edges_csv,
            args.memory_limit,
            args.reimport,
        )
        if imported and args.delete_csv_after_import:
            nodes_csv.unlink(missing_ok=True)
            edges_csv.unlink(missing_ok=True)

        pagerank, timings, total_seconds = run_pagerank(
            args.duckdb.resolve(),
            args.extension.resolve(),
            database,
            args.memory_limit,
            args.max_iterations,
            args.tolerance,
        )
        result = {
            "scale": scale,
            "edge_factor": EDGE_FACTOR,
            "memory_limit": args.memory_limit,
            "estimated_native_bytes": estimated,
            "generation_seconds": generation_seconds,
            "import_process_seconds": import_seconds,
            "cold_pagerank_and_csr_seconds": timings.get("pagerank_cold"),
            "pagerank_seconds": timings.get("pagerank"),
            "csr_and_pagerank_process_seconds": total_seconds,
            **pagerank,
            "database": str(database),
        }
        results = [item for item in results if item.get("scale") != scale]
        results.append(result)
        results.sort(key=lambda item: item["scale"])
        write_results(results_path, results)
        print(
            f"Scale {scale} complete: CSR {human_bytes(result['csr_bytes'])}, "
            f"cold CSR+PageRank {result['cold_pagerank_and_csr_seconds']:.3f}s, "
            f"warm PageRank {result['pagerank_seconds']:.3f}s, "
            f"{result['iterations']} iterations, converged={result['converged']}"
        )

    print(f"\nResults: {results_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (FileNotFoundError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
