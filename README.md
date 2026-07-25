# DuckGQL

[![Main Extension Distribution Pipeline](https://github.com/rahul-iyer/duckdb-gql/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/rahul-iyer/duckdb-gql/actions/workflows/MainDistributionPipeline.yml)

DuckGQL is an experimental C++17 extension that adds a growing subset of
[ISO/IEC 39075:2024 GQL](https://www.iso.org/standard/76120.html) to DuckDB.
It combines graph pattern queries and mutations with DuckDB's native relational
storage and execution engine, plus an explicit CSR layer for graph algorithms.

> [!IMPORTANT]
> DuckGQL is under active development and is not yet a complete or conforming
> ISO GQL implementation. Use the
> [conformance manifest](test/conformance/iso-gql-2024.tsv), not parser
> coverage, as the source of truth for implemented language families.

## What works today

- Managed property graphs backed by typed DuckDB vertex and edge tables.
- Graph-header CSV, compressed CSV, and Parquet bulk import.
- Directed `MATCH`, `OPTIONAL MATCH`, filtering, projection, aggregation,
  ordering, paging, fixed multi-hop paths, and a bounded variable-length path
  subset.
- Standalone node and directed-path `INSERT`, fixed directed
  `MATCH`-and-`INSERT`, property `SET` and `REMOVE`, and edge/node deletion.
- Explicit CSR-backed BFS, DFS, unweighted SSSP, PageRank, weak and strong
  components, degree, closeness, local clustering coefficient, and triangle
  counting.
- Caller-controlled DuckDB transactions for graph queries and mutations, plus
  persistence, vectorized scans, joins, aggregation, and recursive CTE
  execution beneath the GQL layer.

## Build from source

DuckGQL currently targets DuckDB `v1.5.4` and uses ANTLR `4.13.2`.

Prerequisites:

- Git
- CMake
- A C++17 compiler
- Python 3
- Make

Clone the repository with its submodules and build a release binary:

```sh
git clone --recurse-submodules https://github.com/rahul-iyer/duckdb-gql.git
cd duckdb-gql

make setup-vcpkg
VCPKG_TOOLCHAIN_PATH="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" make release
```

The build produces:

```text
build/release/duckdb
build/release/extension/duckgql/duckgql.duckdb_extension
```

The locally built DuckDB shell has DuckGQL preloaded:

```sh
./build/release/duckdb
```

## Prebuilt artifacts

The GitHub Actions distribution workflow builds platform-specific extension
artifacts. DuckDB extensions are tied to both a DuckDB version and a target
platform, so download the artifact matching DuckDB `v1.5.4` and your operating
system architecture.

Development artifacts are unsigned. Start a matching DuckDB CLI with unsigned
extensions enabled:

```sh
duckdb -unsigned
```

Then load the downloaded binary:

```sql
LOAD '/absolute/path/to/duckgql.duckdb_extension';
```

Only load native extension binaries from a source you trust. DuckGQL is not
currently published in DuckDB's core or community extension repository.

## Quick start

DuckGQL imports one vertex file and one edge file. Column names carry the graph
roles and optional property types.

`nodes.csv`:

```csv
personId:ID(People),name:string,age:int,:LABEL
p1,Ada,42,Person
p2,Grace,37,Person
```

`relationships.csv`:

```csv
:START_ID(People),:END_ID(People),:TYPE,since:int
p1,p2,KNOWS,2020
```

Create, load, select, and query the graph:

```sql
CREATE GRAPH social ANY;

COPY GRAPH social FROM (
    VERTICES 'nodes.csv',
    EDGES 'relationships.csv'
) FORMAT GRAPH;

SESSION SET GRAPH social;

MATCH (a:Person)-[e:KNOWS]->(b:Person)
WHERE e.since >= 2020
RETURN a.name AS source_name,
       e.since AS since,
       b.name AS target_name;
```

`COPY GRAPH` accepts `.csv`, `.csv.gz`, `.csv.zst`, and `.parquet`. Validation
is enabled by default and rejects missing or duplicate vertex IDs and missing
edge endpoints. Trusted inputs can skip those validation scans:

```sql
COPY GRAPH social FROM (
    VERTICES 'nodes.parquet',
    EDGES 'relationships.parquet'
) FORMAT GRAPH OPTIONS (VALIDATE FALSE);
```

`DROP GRAPH social` removes both the graph metadata and its managed tables.
`CREATE GRAPH`, `DROP GRAPH`, and `COPY GRAPH` are lifecycle operations that
must run in autocommit mode; DuckGQL rejects them inside an explicit
transaction. Graph queries and mutations can participate in an explicit
caller-controlled DuckDB transaction.

## Explain query plans

DuckGQL queries use DuckDB's native plan renderer after GQL has been lowered.

```sql
EXPLAIN MATCH (person:Person)
WHERE person.age >= 35
RETURN person.name;

EXPLAIN ANALYZE MATCH (person:Person)
RETURN person.name;

EXPLAIN (FORMAT JSON) MATCH (person:Person)
RETURN person.name;
```

`EXPLAIN ANALYZE` executes the query and includes runtime measurements.
Algorithm calls can be inspected after building their CSR snapshot:

```sql
CALL gql_build_csr('social');

EXPLAIN CALL algo.pagerank('social')
YIELD vertex_id, rank
RETURN vertex_id, rank;
```

## Graph algorithms

Graph algorithms require an explicit, derived CSR snapshot. `MATCH` keeps
DuckDB tables authoritative and uses relational/recursive operators, but can
also consume a valid snapshot's node-label postings and selective fixed-hop
adjacency. If no current snapshot exists, the same query falls back to table
scans and joins.

```sql
CALL gql_build_csr('social');

CALL algo.bfs('social', 1, direction := 'out', max_depth := 4);

CALL algo.pagerank(
    'social',
    damping := 0.85,
    max_iterations := 100,
    tolerance := 1e-8
)
YIELD vertex_id, rank
RETURN vertex_id, rank
ORDER BY rank DESC;
```

CSR snapshots are connection-local and version checked. Graph mutations and
direct SQL writes to a graph's vertex or edge tables invalidate the affected
snapshot instead of allowing an algorithm to use stale data. Run
`CALL gql_build_csr('social')` again before the next algorithm call. CSR
construction and CSR algorithms must run in autocommit mode.

Weighted SSSP is not implemented.

Inspection helpers:

```sql
SELECT * FROM gql_graphs();
CALL gql_neighbors('social', 1, 'out');
SELECT * FROM gql_csr_stats('social');
```

## Storage model

DuckGQL uses one canonical storage model:

```text
gql_data.graph_<id>_vertices   typed vertex properties and labels
gql_data.graph_<id>_edges      typed edge properties, types, and endpoints
gql_internal.*                 graph catalog and column mappings
```

The private catalog contains metadata only; vertices, edges, labels, and
properties are not stored as entity-attribute-value rows. The managed tables
remain ordinary DuckDB relations and are authoritative for querying and
mutation. Nodes retain their complete native `VARCHAR[]` label set. Each edge
has exactly one scalar, immutable type. A CSR build derives connection-local
topology and node-label posting lists from those tables; it does not create a
second authoritative store.

## SF10 engineering benchmark

On the 29,987,835-node / 178,561,949-edge LDBC SNB SF10 projection, all seven
currently supported read queries matched their ordered SQL reference rows.
The selective label-posting path reduced Complex 8 from 33.960 ms to
11.900 ms and cumulative rows scanned from 30,602,235 to 802,925. This is an
engineering run, not an official LDBC driver score.

| Query | Previous CSR | Selective label postings |
|---|---:|---:|
| Short 1 | 11.694 ms | 10.397 ms |
| Short 3 | 11.513 ms | 10.628 ms |
| Short 4 | 1.380 ms | 1.351 ms |
| Short 5 | 2.922 ms | 2.535 ms |
| Short 7 | 49.460 ms | 47.843 ms |
| Complex 2 | 128.032 ms | 130.036 ms |
| Complex 8 | 33.960 ms | 11.900 ms |

Reproduce against an existing SF10 graph database:

```sh
python3 scripts/benchmark/benchmark_snb_gql_interactive.py \
  --graph-database build/benchmarks/snb10/snb10-gql.duckdb \
  --output build/benchmarks/snb10/gql-interactive-results-label-postings-point.json
```

DuckLake tables can be used as an input source by exporting graph-header
relations to Parquet and loading those files with `COPY GRAPH`. Direct
zero-copy DuckLake-backed graphs are not supported yet because DuckGQL
currently owns its native graph tables and internal catalog. A future
read-only referenced-table mode is planned.

## Current limitations

- ISO GQL feature families remain partial or planned; grammar recognition does
  not imply semantic or transactional conformance.
- `CREATE GRAPH`, `DROP GRAPH`, and the full-load `COPY GRAPH` operation are
  autocommit-only. `COPY GRAPH` requires an empty graph.
- CSR construction and CSR algorithms are autocommit-only and snapshots are
  local to the connection that built them.
- Bulk import currently accepts one vertex file, one edge file, at most one
  scalar vertex label column, and one edge type column. Every relationship row
  must contain exactly one non-empty type.
- Multiple-path and undirected insertion, general runtime property maps, and
  open-graph schema evolution remain incomplete.
- Path modes, searches, shortest-path groups, query composition, procedure
  semantics, graph types, and the complete GQL value/type system remain
  incomplete.
- Direct registration of existing DuckDB or DuckLake tables as a graph is not
  exposed.

The machine-readable status is
[`test/conformance/iso-gql-2024.tsv`](test/conformance/iso-gql-2024.tsv). The
strict release gate intentionally fails until every applicable family is
complete:

```sh
python3 scripts/gql_conformance/check_gql_conformance.py --release
```

## Development

Build and run the active SQLLogicTest suite:

```sh
VCPKG_TOOLCHAIN_PATH="$PWD/vcpkg/scripts/buildsystems/vcpkg.cmake" make debug
./build/debug/test/unittest "test/sql/gql*"
```

Validate the conformance manifest, grammar inventory, and fixture adapters:

```sh
python3 scripts/gql_conformance/check_gql_conformance.py
```

Run the repository's code-quality checks:

```sh
make format-check
```

## License

DuckGQL is available under the [MIT License](LICENSE).
