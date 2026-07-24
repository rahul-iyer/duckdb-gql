# LiveJournal native-query benchmark

Measured on 2026-07-23 with the Release build on arm64 macOS, 15 DuckDB
threads, and a warm filesystem cache. The managed graph was loaded once with
`COPY GRAPH ... OPTIONS (VALIDATE FALSE)` before profiling. Load time and
checkpoint time are excluded from every query measurement.

The input is SNAP's directed `soc-LiveJournal1`: 4,847,571 vertices and
68,993,773 directed edges. User `10009` is the deterministic stress seed; a
full edge scan selected it as the maximum-out-degree vertex with 20,293
outgoing `follows` edges.

Each result is the median of 10 measured JSON profiles after 2 warmups. SQL and
GQL queries use the same managed native vertex and edge tables, labels, edge
types, source predicate, and directed trail semantics.

## Fixed-query results

| Workload | Result paths | Median latency | Paths/s | Relative to SQL | Peak RSS |
|---|---:|---:|---:|---:|---:|
| Native SQL, 1-hop count | 20,293 | 7.483 ms | 2.71M | 1.000x | 119.2 MiB |
| Table-backed GQL, 1-hop count | 20,293 | 8.720 ms | 2.33M | 1.165x | 132.2 MiB |
| Native SQL, exact 2-hop trail count | 855,573 | 68.119 ms | 12.56M | 1.000x | 462.8 MiB |
| Table-backed GQL, exact 2-hop trail count | 855,573 | 77.027 ms | 11.11M | 1.131x | 480.7 MiB |

The exact two-hop GQL plan is within 13.1% of matched handwritten SQL while
counting 855,573 paths. The smaller one-hop query exposes a roughly 1.24 ms
fixed planning/execution overhead and finishes below 9 ms.

The measured GQL statements are:

```sql
SESSION SET GRAPH livejournal;

MATCH (source:user)-[:follows]->(target:user)
WHERE source.external_id = '10009'
RETURN count(*);

MATCH (source:user)-[:follows]->{2}(target:user)
WHERE source.external_id = '10009'
RETURN count(*);
```

## Variable-length result

Both queries return the first 100 directed different-edge trail rows. Source
property equality is pushed into the recursive anchor, so execution starts at
user `10009` rather than all 4.8 million vertices.

| Workload | Returned paths | Median latency | Paths/s | Relative latency | Peak RSS |
|---|---:|---:|---:|---:|---:|
| Handwritten DuckDB recursive SQL, `UBIGINT[]` trail | 100 | 22.795 ms | 4.39K | 1.000x | 494.4 MiB |
| Native table-backed GQL VLP | 100 | 23.764 ms | 4.21K | 1.043x | 516.3 MiB |

The GQL compiler generates a native DuckDB recursive CTE over the managed
vertex and edge tables. Its 0.97 ms median overhead includes GQL expression and
result-shaping layers; no CSR snapshot is involved.

The measured GQL statement is:

```sql
MATCH (source:user)-[:follows]->*(target:user)
WHERE source.external_id = '10009'
RETURN target.external_id
LIMIT 100;
```

## Long-running variable-length result

The longer workload uses source user `1000018`, whose single outgoing edge
forces deeper recursive iterations before producing the first 1,000,000 trail
rows. This avoids timing a shallow, high-fan-out first frontier. The handwritten
SQL and generated GQL both preserve managed edge identity as `UBIGINT[]` trail
state. The table was re-measured on 2026-07-23 with the Release build and is the
median of 5 measured profiles after 1 warmup, using 15 threads and a warm
database. Both plans returned exactly 1,000,000 rows in every measured run.

| Workload | Returned paths | Median latency | Measured range | Paths/s | Relative latency | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|
| Handwritten DuckDB recursive SQL, `UBIGINT[]` trail | 1,000,000 | 0.932 s | 0.870-1.012 s | 1.07M | 1.000x | 6,217.5 MiB |
| Native table-backed GQL VLP | 1,000,000 | 0.995 s | 0.827-1.096 s | 1.01M | 1.067x | 6,048.9 MiB |

Against the previous matched `VARCHAR[]` GQL baseline, native managed edge
identity reduced median latency by 21.4% and process peak RSS by 33.4%. GQL is
within 6.7% of matched handwritten SQL on latency and used 2.7% less peak RSS.
Registered non-managed tables retain the generic `VARCHAR[]` fallback because
their key type is not constrained to the managed `UBIGINT` representation.

### Operator-profile diagnosis

The final long-run JSON retains every measured DuckDB operator tree. Median
profile counters confirm that both plans now execute the same recursive work:

| Counter | Handwritten SQL | GQL |
|---|---:|---:|
| Total query CPU time | 13.54 s | 14.89 s |
| Recursive CTE subtree CPU time | 13.31 s | 14.65 s |
| Recursive CTE output rows | 1,000,166 | 1,000,166 |
| Recursive subtree rows scanned | 394,252,480 | 394,182,848 |

The root cause of the prior 7x result was recursive `UNION` duplicate
elimination. Each path state already contains its complete ordered sequence of
globally unique edge keys, so distinct trails cannot produce identical state;
hashing every growing `VARCHAR[]` was redundant. The compiler now requests
`UNION ALL`, matching the handwritten query.

An earlier experiment incorrectly reported that `UNION ALL` did not help. The
benchmark CLI contains a statically linked GQL extension, and rebuilding only
the loadable extension did not replace that code even though the runner issued
`LOAD <path>`. Temporary physical-plan metadata proved that the stale shell was
still executing `Union All=false`. Rebuilding the `shell` target produced the
correct plan and removed the gap. The runner now detects
`STATICALLY_LINKED` mode, refuses a CLI older than the GQL sources, skips the
misleading load command in that mode, and records the install mode in JSON.

## CSR construction status

Prepared CSR was not built in this LiveJournal run, so this report makes no
claim about traversal-only CSR latency on that dataset.

The builder was subsequently changed to two streamed edge passes: one counts
degrees and allocates final arrays, and one scatters edges into outgoing and
incoming CSR. Managed graph IDs also avoid the generic edge-ID validation hash
set. A later Graphalytics datagen run successfully built 82,050,510 stored arcs
in 5.685 seconds with 7.28 GiB peak process RSS. That result validates the
current scale direction, but it is a different workload and does not replace a
future LiveJournal CSR measurement.

## Reproduce

Create the managed query database outside measurement:

```sql
CREATE GRAPH livejournal ANY;

COPY GRAPH livejournal FROM (
    VERTICES 'data/livejournal/graph-import/nodes.csv',
    EDGES 'data/livejournal/graph-import/edges.csv'
) FORMAT GRAPH OPTIONS (VALIDATE FALSE);

CHECKPOINT;
```

Then run the profiles:

```sh
cmake --build build/release --target shell duckgql_loadable_extension

python3 scripts/benchmark_livejournal_queries.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/duckgql/duckgql.duckdb_extension \
  --database build/benchmarks/livejournal-query-ubigint.duckdb \
  --seed 10009 \
  --runs 10 \
  --warmups 2 \
  --threads 15 \
  --output build/benchmarks/livejournal-query-ubigint-current.json
```

The ignored JSON output preserves every measured latency, CPU time, result-path
count, throughput calculation, and process peak-RSS measurement.

Run only the long VLP comparison with:

```sh
python3 scripts/benchmark_livejournal_queries.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/duckgql/duckgql.duckdb_extension \
  --database build/benchmarks/livejournal-query.duckdb \
  --seed 1000018 \
  --one-hop-paths 1 \
  --recursive-paths 1000000 \
  --vlp-only \
  --runs 5 \
  --warmups 1 \
  --threads 15 \
  --include-operator-profiles \
  --output build/benchmarks/livejournal-vlp-long-ubigint-current.json
```
