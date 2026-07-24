# LDBC Graphalytics datagen-7_8-zf local run

Generated: 2026-07-24T01:31:25.266665+00:00

Local run over the official dataset and reference outputs; this is not an audited Graphalytics submission.

- Graph: 16,521,886 vertices / 41,025,255 undirected logical edges
- Stored CSR arcs: 82,050,510
- Threads: 8
- COPY GRAPH: 36.549s
- CSR build: 5.239s
- Accounted CSR memory: 2.27 GiB
- Reduction from legacy principal-array layout: 37.2%
- Neighbor ordinal width: 4 bytes
- Peak process RSS: 8.77 GiB
- CSR-build-only peak RSS: 3.22 GiB
- CSR-build incremental peak above an idle database: 3.20 GiB
- CSR-build accounting coverage: 82.4%

The compact layout is 37.2% smaller than the previous principal-array
representation, exceeding the P0.3 25% gate. CSR construction improved from
5.685s to 5.239s (7.8% faster). Accounted resident and build-only allocations
explain 82.4% of the incremental build-process peak; the remaining 17.6%
contains DuckDB execution buffers, allocator overhead, and runtime state.

| Kernel | Validation | Warm median | Min | Max |
|---|---:|---:|---:|---:|
| BFS | PASS | 4.869s | 4.866s | 5.113s |
| PR | PASS | 5.974s | 5.959s | 6.276s |
| WCC | PASS | 4.086s | 4.068s | 4.168s |
| LCC | PASS | 29.875s | 29.771s | 29.936s |

CDLP is not implemented. The current DuckGQL SSSP is unweighted and is not run against Graphalytics weighted SSSP.

## Reproduce

```sh
python3 scripts/benchmark_graphalytics_scale.py --dataset datagen-7_8-zf --threads 8 --warmups 1 --runs 3
```
