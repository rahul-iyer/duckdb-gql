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
- DuckDB transactions, persistence, vectorized scans, joins, aggregation, and
  recursive CTE execution beneath the GQL layer.

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

Graph algorithms use an explicit, derived CSR snapshot. Ordinary `MATCH`
queries continue to use DuckDB's relational and recursive operators.

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

CSR snapshots are connection-local and version checked. Rebuild the snapshot
after graph mutations or direct SQL topology changes. Weighted SSSP and using
CSR as an automatic `MATCH` execution backend are not implemented.

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
mutation.

DuckLake tables can be used as an input source by exporting graph-header
relations to Parquet and loading those files with `COPY GRAPH`. Direct
zero-copy DuckLake-backed graphs are not supported yet because DuckGQL
currently owns its native graph tables and internal catalog. A future
read-only referenced-table mode is planned.

## Current limitations

- ISO GQL feature families remain partial or planned; grammar recognition does
  not imply semantic or transactional conformance.
- `COPY GRAPH` is a full-load, autocommit-only operation for an empty graph.
- Bulk import currently accepts one vertex file, one edge file, at most one
  scalar vertex label column, and one edge type column.
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
