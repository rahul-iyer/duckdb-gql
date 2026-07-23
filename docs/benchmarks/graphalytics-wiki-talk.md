# LDBC Graphalytics wiki-Talk local run

Generated: 2026-07-23T02:14:26.018696+00:00

Local run over the official dataset and reference outputs; this is not an audited Graphalytics submission.

- Graph: 2,394,385 vertices / 5,021,410 directed edges
- Threads: 8
- COPY GRAPH: 2.608s
- CSR build: 0.405s
- Peak process RSS: 1.46 GiB

| Kernel | Validation | Warm median | Min | Max |
|---|---:|---:|---:|---:|
| BFS | PASS | 0.395s | 0.394s | 0.423s |
| PR | PASS | 0.668s | 0.667s | 0.727s |
| WCC | PASS | 0.456s | 0.452s | 0.480s |
| LCC | PASS | 4.579s | 4.566s | 4.620s |

CDLP is not implemented. Weighted SSSP is not present in wiki-Talk and DuckGQL's current SSSP is unweighted.

## Reproduce

```sh
python3 scripts/benchmark_graphalytics_scale.py --threads 8 --warmups 1 --runs 3
```
