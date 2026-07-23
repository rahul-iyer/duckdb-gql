# LDBC Graphalytics datagen-7_8-zf local run

Generated: 2026-07-23T02:39:21.695883+00:00

Local run over the official dataset and reference outputs; this is not an audited Graphalytics submission.

- Graph: 16,521,886 vertices / 41,025,255 undirected logical edges
- Stored CSR arcs: 82,050,510
- Threads: 8
- COPY GRAPH: 36.948s
- CSR build: 5.685s
- Peak process RSS: 7.28 GiB

| Kernel | Validation | Warm median | Min | Max |
|---|---:|---:|---:|---:|
| BFS | PASS | 4.830s | 4.788s | 4.970s |
| PR | PASS | 5.667s | 5.571s | 5.853s |
| WCC | PASS | 4.075s | 4.051s | 4.174s |
| LCC | PASS | 29.476s | 29.123s | 30.264s |

CDLP is not implemented. The current duckdb-gql SSSP is unweighted and is not run against Graphalytics weighted SSSP.

## Reproduce

```sh
python3 scripts/benchmark_graphalytics_scale.py --dataset datagen-7_8-zf --threads 8 --warmups 1 --runs 3
```
