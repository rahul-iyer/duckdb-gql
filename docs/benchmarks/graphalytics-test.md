# LDBC Graphalytics test-dataset validation

Generated: 2026-07-23T02:10:07.061024+00:00

This is a local compatibility run over the official Graphalytics test archives and bundled reference outputs. It is not an audited or published Graphalytics benchmark submission.

- CLI: `build/release/duckdb`
- Extension: `build/release/extension/duckgql/duckgql.duckdb_extension`
- Threads: 8
- Warmups / measured runs: 2 / 7
- Passing cases: 8 / 8

| Dataset | Kernel | Shape | Validation | COPY GRAPH | CSR build | Warm median |
|---|---:|---:|---:|---:|---:|---:|
| test-bfs-directed | BFS | 10 V / 17 E | PASS | 0.011000s | 0.002000s | 0.001000s |
| test-bfs-undirected | BFS | 10 V / 14 E | PASS | 0.011000s | 0.002000s | 0.001000s |
| test-pr-directed | PR | 50 V / 246 E | PASS | 0.012000s | 0.002000s | 0.001000s |
| test-pr-undirected | PR | 50 V / 113 E | PASS | 0.012000s | 0.002000s | 0.001000s |
| test-lcc-directed | LCC | 10 V / 17 E | PASS | 0.010000s | 0.002000s | 0.001000s |
| test-lcc-undirected | LCC | 9 V / 12 E | PASS | 0.011000s | 0.002000s | 0.001000s |
| test-wcc-directed | WCC | 8 V / 10 E | PASS | 0.011000s | 0.002000s | 0.001000s |
| test-wcc-undirected | WCC | 8 V / 7 E | PASS | 0.010000s | 0.002000s | 0.001000s |

## Contract coverage

- BFS: exact match; absent traversal rows are converted to Graphalytics infinity.
- PageRank and LCC: Graphalytics relative epsilon validation (`0.0001`).
- WCC: partition-equivalence validation with a bijection between component labels.
- Undirected datasets: each logical input edge is expanded to two directed CSR arcs.
- CDLP: not run because DuckGQL does not implement this kernel.
- SSSP: not run because DuckGQL currently implements unweighted hop distance, while Graphalytics requires double edge weights.

## Failures

None.
## Reproduce

```sh
python3 scripts/benchmark_graphalytics.py --threads 8 --warmups 2 --runs 7
```

Specification: https://ldbcouncil.org/ldbc_graphalytics_docs/graphalytics_spec.pdf
