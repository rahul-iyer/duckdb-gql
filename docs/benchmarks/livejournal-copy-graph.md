# LiveJournal `COPY GRAPH` benchmark

Measured on 2026-07-19 with the Release build on arm64 macOS, 15 DuckDB
threads, and a warm filesystem cache. The input is SNAP's directed
[`soc-LiveJournal1`](https://snap.stanford.edu/data/soc-LiveJournal1.html):
4,847,571 vertices and 68,993,773 directed edges, or 73,841,344 graph rows.

The official 259,619,239-byte gzip had SHA-256
`d7bcd5a87b88c896c35fdb9611e804c3f4033c39b58c4c9ea3ba53c680d516d8`.
It was converted once to a 1,625,461,646-byte graph-header CSV pair. Download,
decompression, and conversion are offline and excluded from every load
measurement.

The normalized EAV compatibility backend is intentionally excluded. The
comparison is equivalent native DuckDB wide-table SQL versus the managed
native tables created by `COPY GRAPH`.

## Results

Load latency excludes the separately measured final `CHECKPOINT`. Each result
is the median of 10 measured runs after 2 warmups, using a fresh v1.5 database
and a randomized case order.

| Load path | Median load | Graph rows/s | Relative latency | Database size | Peak RSS |
|---|---:|---:|---:|---:|---:|
| Equivalent native DuckDB SQL | 19.710 s | 3.75M | 1.000x | 307.01 MiB | 4.11 GiB |
| `COPY GRAPH`, trusted input | 19.876 s | 3.72M | 1.008x | 310.51 MiB | 4.16 GiB |
| `COPY GRAPH`, strict validation | 23.615 s | 3.13M | 1.198x | 310.51 MiB | 4.29 GiB |

The trusted-input path retains 99.16% of native load throughput and adds only
166 ms at the median. Strict validation retains 83.46% of native throughput;
exact node-ID uniqueness and relationship endpoint checks add 3.739 seconds
relative to the trusted path. Median checkpoint time is 7 ms for native tables
and 8 ms for both graph cases.

The native baseline performs the same physical work as `COPY GRAPH`: two
DuckDB CSV scans into staging tables, generated stable IDs, native label/type
columns, and two endpoint joins. It omits only graph catalog management and
optional validation.

## Measured graph load

```sql
CREATE GRAPH livejournal ANY;

COPY GRAPH livejournal FROM (
    VERTICES 'data/livejournal/graph-import/nodes.csv',
    EDGES 'data/livejournal/graph-import/edges.csv'
) FORMAT GRAPH OPTIONS (VALIDATE FALSE);
```

Omit `OPTIONS (VALIDATE FALSE)` for strict validation. Fast mode should be used
only when the input producer guarantees non-null unique node IDs, non-empty
relationship types, and valid endpoints.

## Reproduce

```sh
mkdir -p data/livejournal
curl -L https://snap.stanford.edu/data/soc-LiveJournal1.txt.gz \
  -o data/livejournal/soc-LiveJournal1.txt.gz

cmake --build build/release --target shell duckgql_loadable_extension -j8

python3 scripts/benchmark_copy_graph.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/duckgql/duckgql.duckdb_extension \
  --source data/livejournal/soc-LiveJournal1.txt.gz \
  --prepared-dir data/livejournal/graph-import \
  --dataset-name soc-LiveJournal1 \
  --skip-rows 4 \
  --vertex-label user \
  --edge-type follows \
  --expected-vertices 4847571 \
  --expected-edges 68993773 \
  --runs 10 \
  --warmups 2 \
  --threads 15 \
  --output build/benchmarks/copy-graph-livejournal-current.json
```

The runner creates the graph-header CSV pair once if absent, before timing. The
ignored JSON output preserves every trial timing, checkpoint, row count,
database size, and process peak-RSS measurement.
