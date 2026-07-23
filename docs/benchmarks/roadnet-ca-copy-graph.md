# roadNet-CA `COPY GRAPH` benchmark

Measured on 2026-07-19 with the Release build on arm64 macOS, 15 DuckDB
threads, and a warm filesystem cache. The workload contains 1,965,206
vertices and 5,533,214 directed edges (7,498,420 graph rows) in a
150,082,167-byte Neo4j-format CSV pair derived from SNAP `roadNet-CA`.
Preparing that pair is offline and excluded from every measurement.

The normalized EAV compatibility backend is intentionally excluded. The
comparison is between equivalent native DuckDB wide-table SQL and the managed
native tables created by `COPY GRAPH`.

## Results

Load latency excludes the separately measured final `CHECKPOINT`. Each number
is the median of 10 measured runs after 2 warmups, using a fresh database and a
randomized case order.

| Load path | Median load | Graph rows/s | Relative latency | Database size | Peak RSS |
|---|---:|---:|---:|---:|---:|
| Equivalent native DuckDB SQL | 2.030 s | 3.69M | 1.000x | 46.51 MiB | 682.45 MiB |
| `COPY GRAPH`, trusted input | 2.093 s | 3.58M | 1.031x | 50.01 MiB | 762.19 MiB |
| `COPY GRAPH`, strict validation | 2.480 s | 3.02M | 1.222x | 50.01 MiB | 772.86 MiB |

The trusted-input path retains 97.0% of native load throughput and adds 63 ms
at the median. Strict validation retains 81.8% of native throughput; its exact
node-ID uniqueness and relationship endpoint checks account for the extra
388 ms relative to the trusted path. Median checkpoint time is 1 ms for native
tables and 2 ms for both graph cases.

The native baseline deliberately performs the same physical work as
`COPY GRAPH`: two DuckDB CSV scans into staging tables, generated stable IDs,
native label/type columns, and two endpoint joins. It omits only graph catalog
management and optional validation.

## Measured command

The fast managed load is:

```sql
CREATE GRAPH roadnet ANY;

COPY GRAPH roadnet FROM (
    VERTICES 'data/roadnet-ca/neo4j/nodes.csv',
    EDGES 'data/roadnet-ca/neo4j/edges.csv'
) FORMAT NEO4J OPTIONS (VALIDATE FALSE);
```

Omit `OPTIONS (VALIDATE FALSE)` to use strict validation. Fast mode should be
used only when the input producer already guarantees non-null unique node IDs,
non-empty relationship types, and valid endpoints.

## Reproduce

Download the official dataset and build the Release shell and extension:

```sh
mkdir -p data/roadnet-ca
curl -L https://snap.stanford.edu/data/roadNet-CA.txt.gz \
  -o data/roadnet-ca/roadNet-CA.txt.gz

cmake --build build/release --target shell duckgql_loadable_extension -j8
```

Run the benchmark:

```sh
python3 scripts/benchmark_copy_graph.py \
  --duckdb build/release/duckdb \
  --extension build/release/extension/duckgql/duckgql.duckdb_extension \
  --source data/roadnet-ca/roadNet-CA.txt.gz \
  --prepared-dir data/roadnet-ca/neo4j \
  --runs 10 \
  --warmups 2 \
  --threads 15 \
  --output build/benchmarks/copy-graph-roadnet-current.json
```

The runner creates the Neo4j CSV pair once if it is absent, before timing. The
ignored JSON output preserves all trial timings, database sizes, row counts,
and process peak-RSS measurements.
