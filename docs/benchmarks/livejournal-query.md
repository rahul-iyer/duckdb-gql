# LiveJournal native-query benchmark

Measured on 2026-07-19 with the Release build on arm64 macOS, 15 DuckDB
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
| Native SQL, 1-hop count | 20,293 | 7.830 ms | 2.59M | 1.000x | 124.9 MiB |
| Table-backed GQL, 1-hop count | 20,293 | 8.710 ms | 2.33M | 1.112x | 136.1 MiB |
| Native SQL, exact 2-hop trail count | 855,573 | 63.511 ms | 13.47M | 1.000x | 461.3 MiB |
| Table-backed GQL, exact 2-hop trail count | 855,573 | 64.841 ms | 13.19M | 1.021x | 491.8 MiB |

The exact two-hop GQL plan is within 2.1% of matched handwritten SQL while
counting 855,573 paths. The smaller one-hop query exposes a roughly 0.88 ms
fixed planning/execution overhead, but still finishes below 9 ms.

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
| Handwritten DuckDB recursive SQL, `VARCHAR[]` trail | 100 | 22.436 ms | 4.46K | 1.000x | 507.0 MiB |
| Native table-backed GQL VLP | 100 | 23.576 ms | 4.24K | 1.051x | 522.9 MiB |

The GQL compiler generates a native DuckDB recursive CTE over the managed
vertex and edge tables. Its 1.14 ms median overhead includes GQL expression and
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
SQL uses the same `VARCHAR[]` trail state and casts each `UBIGINT` edge key to
`VARCHAR` for `list_contains` and `list_append`, matching the GQL edge-identity
representation. The table is the median of 5 measured profiles after 1 warmup,
still using 15 threads and the same warm database. Every measured execution on
both sides exceeded one second.

| Workload | Returned paths | Median latency | Measured range | Paths/s | Relative latency | Peak RSS |
|---|---:|---:|---:|---:|---:|---:|
| Handwritten DuckDB recursive SQL, `VARCHAR[]` trail | 1,000,000 | 1.235 s | 1.228-1.404 s | 809.72K | 1.000x | 8,620.7 MiB |
| Native table-backed GQL VLP | 1,000,000 | 1.266 s | 1.214-1.375 s | 789.92K | 1.025x | 9,078.2 MiB |

The earlier type-isolation run raised handwritten SQL from 857 ms with
`UBIGINT[]` to 1.160 s with `VARCHAR[]`, so preserving the native key type is
still worthwhile. Once recursive duplicate elimination is removed, however,
GQL is within 2.5% of the matched handwritten SQL on the long workload.

### Operator-profile diagnosis

The final long-run JSON retains every measured DuckDB operator tree. Median
profile counters confirm that both plans now execute the same recursive work:

| Counter | Handwritten SQL | GQL |
|---|---:|---:|
| Total query CPU time | 18.57 s | 19.06 s |
| Recursive CTE subtree CPU time | 18.35 s | 18.84 s |
| Recursive CTE output rows | 1,000,166 | 1,000,166 |
| Recursive subtree rows scanned | 394,176,704 | 394,178,752 |

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

## Remaining CSR construction gap

Prepared CSR was not built in this run. The current table-backed CSR builder
materializes all 69 million edge rows, keeps an edge-ID hash set, and creates
multiple full edge-vector copies while sorting outgoing and incoming order.
That construction is not safely bounded on the 24 GiB benchmark machine.
Because CSR construction is offline, it must be made streaming and
memory-bounded before measuring traversal-only CSR latency at this scale.

## Reproduce

Create the managed query database outside measurement:

```sql
CREATE GRAPH livejournal ANY;

COPY GRAPH livejournal FROM (
    VERTICES 'data/livejournal/neo4j/nodes.csv',
    EDGES 'data/livejournal/neo4j/edges.csv'
) FORMAT NEO4J OPTIONS (VALIDATE FALSE);

CHECKPOINT;
```

Then run the profiles:

```sh
cmake --build build/release --target shell duckgql_loadable_extension

python3 scripts/benchmark_livejournal_queries.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/duckgql/duckgql.duckdb_extension \
  --database build/benchmarks/livejournal-query.duckdb \
  --seed 10009 \
  --runs 10 \
  --warmups 2 \
  --threads 15 \
  --output build/benchmarks/livejournal-query-current.json
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
  --output build/benchmarks/livejournal-vlp-long-current.json
```
