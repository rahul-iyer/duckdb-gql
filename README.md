# DuckGQL

[![Main Extension Distribution Pipeline](https://github.com/rahul-iyer/duckdb-gql/actions/workflows/MainDistributionPipeline.yml/badge.svg)](https://github.com/rahul-iyer/duckdb-gql/actions/workflows/MainDistributionPipeline.yml)

[Documentation](https://duckgql.com/docs/) ·
[Browser playground](https://duckgql.com/)

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
- Read-only, zero-copy typed graph projections over existing DuckDB and
  DuckLake tables, with live or pinned snapshots and snapshot-aware CSR
  invalidation.
- Graph-header CSV, compressed CSV, and Parquet bulk import.
- Directed `MATCH`, `OPTIONAL MATCH`, filtering, projection, aggregation,
  ordering, paging, fixed multi-hop paths, and a bounded variable-length path
  subset.
- Standalone node and directed-path `INSERT`, fixed directed
  `MATCH`-and-`INSERT`, property `SET` and `REMOVE`, and edge/node deletion.
- Explicit CSR-backed BFS, DFS, unweighted SSSP, PageRank, weak and strong
  components, Louvain community detection, degree, closeness, local clustering
  coefficient, and triangle counting.
- Caller-controlled DuckDB transactions for graph queries and mutations, plus
  persistence, vectorized scans, joins, aggregation, and recursive CTE
  execution beneath the GQL layer.

## Install

DuckGQL is available from DuckDB Community Extensions:

```sql
INSTALL duckgql FROM community;
LOAD duckgql;
```

## Build from source

DuckGQL currently targets DuckDB `v1.5.5` and uses ANTLR `4.13.2`.

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
platform, so download the artifact matching DuckDB `v1.5.5` and your operating
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

Only load native extension binaries from a source you trust. For normal use,
prefer installing the signed Community Extensions build above.

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

Graph lifecycle and session commands also accept the standard optional
`PROPERTY` keyword, for example `CREATE PROPERTY GRAPH`,
`DROP PROPERTY GRAPH`, and `SESSION SET PROPERTY GRAPH`. Clear the selected
graph without changing its data or catalog entry with `SESSION RESET GRAPH` or
`SESSION RESET PROPERTY GRAPH`.

`RETURN` aliases may use backtick or double-quote delimiters when the output
name contains spaces or escaped delimiters, for example
`` RETURN a.name AS `Person Name` ``.

An explicit inline typed schema can instead be persisted with the graph:

```sql
CREATE GRAPH social_typed TYPED {
    (Person :Person {id INT64 NOT NULL, name STRING}),
    (Person)-[:KNOWS {since INT32}]->(Person)
};
```

Typed creation records and validates the schema and immediately materializes
empty managed vertex and edge tables. Select the graph and insert into it
without a separate `COPY GRAPH` step:

```sql
SESSION SET GRAPH social_typed;

INSERT (alice:Person {id: 1, name: 'Alice'})
       -[:KNOWS {since: 2020}]->
       (bob:Person {id: 2, name: 'Bob'})
RETURN alice;
```

The native columns enforce scalar conversions, and table constraints enforce
declared labels/edge types, allowed properties, and per-type `NOT NULL`
properties. Properties shared by multiple node or edge types must map to the
same DuckDB physical type.

`INSERT RETURN` can return one or more directly inserted node and edge
variables. Each result has the same graph-element struct representation as
`MATCH`, including its generated identity, labels or edge type, endpoints, and
mapped properties. Node and edge values can be mixed, and result columns
preserve the `RETURN` list order. Expressions, ordering, and pagination are not
yet supported on standalone `INSERT RETURN`.

Standalone `INSERT` also accepts multiple comma-separated directed paths in a
single mutation, including paths containing edges:

```sql
INSERT (a:Person {id: 3, name: 'A'})-[:KNOWS]->(b:Person {id: 4, name: 'B'}),
       (c:Person {id: 5, name: 'C'})-[:KNOWS]->(d:Person {id: 6, name: 'D'})
RETURN a, d;
```

`LET` accepts both `LET x = expression` and the equivalent untyped
`LET VALUE x = expression` form. `BYTE_LENGTH` and `OCTET_LENGTH` return the
number of bytes in a byte string such as `X'00 FF'`. `PATH_LENGTH` returns the
number of edges in an already-bound named fixed path:

```sql
MATCH p = (a:Person)-[:KNOWS]->(b:Person)
LET VALUE source_name = a.name
RETURN source_name, PATH_LENGTH(p), BYTE_LENGTH(X'00 FF');
```

`EXP` returns *e* raised to a numeric argument, while `LN` returns its natural
logarithm. Both functions use DuckDB's floating-point and domain behavior and
preserve `NULL` inputs.

Labels and property names in `MATCH` and `INSERT` may use double-quoted or
backtick-delimited identifiers. Property references use the same forms, so names
such as `person."First Name"` retain their existing identifier spelling.

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
DuckGQL first applies semantic rewrites such as safe predicate pushdown. Once
the selected graph, property indexes, and current CSR snapshot are known, a
graph access-path optimizer chooses table scans, indexed property lookups,
selective node-label postings, and fixed-hop CSR expansions. Relational
lowering only materializes those choices; DuckDB then costs and reorders the
resulting native join graph.

Algorithm calls can be inspected directly. Execution automatically builds the
smallest CSR projection required by the algorithm; an explicit full snapshot is
only needed when inspecting optimizer-selected CSR access paths:

```sql
CALL gql_build_csr('social');

EXPLAIN CALL algo.pagerank('social')
YIELD vertex_id, rank
RETURN vertex_id, rank;
```

## Graph algorithms

Graph algorithms automatically build and cache a derived CSR projection on
first use. Directional topology, edge identity, edge/vertex labels, postings,
and fanout statistics are independent capabilities: each algorithm requests
only the structures required by its output and filters. `MATCH` keeps DuckDB
tables authoritative and uses relational/recursive operators, but can also
consume an explicitly built full snapshot's node-label postings and selective
fixed-hop adjacency. If no current full snapshot exists, the same query falls
back to table scans and joins.

```sql
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

CALL algo.louvain(
    'social',
    resolution := 1.0,
    max_iterations := 32
)
YIELD vertex_id, community_id, community_size, modularity
RETURN vertex_id, community_id, community_size, modularity
ORDER BY community_size DESC, vertex_id;
```

Louvain currently uses an unweighted simple-undirected projection: reciprocal
and parallel edges are coalesced, and self-loops are ignored.

CSR snapshots are immutable, database-instance scoped, and version checked.
Connections sharing one DuckDB database instance reuse the same derived graph
projection. Graph mutations and direct SQL writes to a graph's vertex or edge
tables invalidate the affected snapshot instead of allowing an algorithm to use
stale data. The next algorithm call rebuilds its required projection
automatically, with concurrent automatic builders coalesced per graph. Run
`gql_build_csr` explicitly only to prepare the full
optimizer/neighbor/inspection snapshot. CSR construction and CSR algorithms
must run in autocommit mode.

Weighted SSSP is not implemented.

Inspection helpers:

```sql
SELECT * FROM gql_graphs();
CALL gql_neighbors('social', 1, 'out');
SELECT * FROM gql_csr_stats('social');
SELECT * FROM gql_csr_edge_stats('social');
```

`gql_csr_stats` reports the smallest current projection and exposes capability
columns such as `has_outgoing`, `has_incoming`, `has_edge_ids`, and
`has_edge_labels`. Its `snapshot_acquisition_count` distinguishes consumer
acquisition from CSR construction; fixed-hop and path expansion operators pin
one immutable snapshot instead of reacquiring it for every lateral seed.
`gql_csr_edge_stats` reports per-type edge counts, active source/target counts,
average directional degree, and maximum directional degree. The graph optimizer
uses these database-scoped statistics to compare a correlated CSR frontier
with a bulk edge-table scan and to avoid treating a highly skewed one-row
endpoint as uniformly selective.

## Query DuckLake tables as a graph

DuckGQL lets you run GQL queries and graph algorithms directly over selected
columns in existing DuckLake tables. Your data stays in DuckLake, and you
choose which columns become source keys and properties. DuckGQL assigns the
dense, snapshot-local element IDs used for graph execution.

```sql
INSTALL ducklake;
LOAD ducklake;
LOAD duckgql;

ATTACH 'ducklake:lakehouse.ducklake' AS lake;

CREATE GRAPH social TYPED {
    (Customer :Customer {id INT64 NOT NULL, name STRING}),
    (ProductNode :ProductNode {id INT64 NOT NULL, name STRING, price FLOAT64}),
    (Customer)-[:BOUGHT {id INT64 NOT NULL, quantity INT32}]->(ProductNode)
}
FROM TABLES (
    VERTEX TABLE lake.main.customers
        MAP TO NODE TYPE Customer
        KEY (customer_id)
        PROPERTIES (
            customer_id AS id,
            customer_name AS name
        ),
    VERTEX TABLE lake.main.products
        MAP TO NODE TYPE ProductNode
        KEY (product_id)
        PROPERTIES (
            product_id AS id,
            product_name AS name,
            price AS price
        ),
    EDGE TABLE lake.main.orders
        MAP TO EDGE TYPE BOUGHT
        SOURCE (customer_id) REFERENCES NODE TYPE Customer
        DESTINATION (product_id) REFERENCES NODE TYPE ProductNode
        PROPERTIES (
            order_id AS id,
            quantity AS quantity
        )
)
OPTIONS (
    SNAPSHOT_POLICY 'LIVE',
    ACCESS_MODE 'READ_ONLY',
    VALIDATE TRUE
);

SESSION SET GRAPH social;

MATCH (customer:Customer)-[order:BOUGHT]->(product:ProductNode)
RETURN customer.name, product.name, order.quantity;

CALL algo.pagerank('social')
YIELD vertex_id, rank
RETURN vertex_id, rank
ORDER BY rank DESC;
```

Only mapped columns are visible through GQL. Each vertex table maps one node
type; edge endpoints resolve through the named node-type mappings. Vertex source
keys need only be unique within their physical node mapping. Edge mappings may
omit `KEY`, as above, or supply a key that is unique within that edge mapping.
`element_id()` returns DuckGQL's generated snapshot-local identity, not the
source key, so applications should persist mapped source-ID properties instead.
With `SNAPSHOT_POLICY 'LIVE'`, queries see the current DuckLake snapshot,
including newly committed data. Graph algorithms refresh automatically when
that snapshot changes.

For reproducible analysis, attach DuckLake at a specific version and register
the graph with a pinned policy:

```sql
ATTACH 'ducklake:lakehouse.ducklake' AS lake (SNAPSHOT_VERSION 42);

-- Use the same typed schema and FROM TABLES mapping shown above.
CREATE GRAPH historical_social TYPED { ... }
FROM TABLES ( ... )
OPTIONS (
    SNAPSHOT_POLICY 'PINNED',
    ACCESS_MODE 'READ_ONLY',
    VALIDATE TRUE
);
```

The pinned graph continues to read snapshot 42. If `lake` is attached at a
different snapshot, DuckGQL asks you to reattach it with the required
`SNAPSHOT_VERSION` instead of returning different results.

DuckLake-backed graphs are currently read-only and support multiple vertex and
edge tables from the same DuckLake catalog, using integer keys. A node type may
be mapped by one vertex table; joining several physical tables into one node
record is not yet supported.

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
has exactly one scalar, immutable type. A CSR build derives database-scoped
topology and node-label posting lists from those tables; it does not create a
second authoritative store.

### Property indexes

Create an index for selective vertex-property equality lookups:

```sql
CALL gql_create_property_index('social', 'id');
SELECT * FROM gql_property_indexes();

MATCH (person:Person)
WHERE person.id = 123
RETURN person.name;

CALL gql_drop_property_index('social', 'id');
```

The index is a native DuckDB ART index over the graph's typed property column,
not a separate graph index. DuckDB maintains it for direct SQL and GQL writes.
The index is graph-wide: if different labels reuse the same property value,
DuckGQL obtains the indexed candidates first and still applies every requested
label exactly. Property indexes work without CSR; when a current CSR snapshot
is present, an indexed vertex lookup can seed a fixed-hop CSR expansion.

## SF10 engineering benchmark

On the 29,987,835-node / 178,561,949-edge LDBC SNB SF10 projection, all
sixteen currently supported Interactive reads match their ordered DuckDB SQL
reference rows. Their directional latency sum is 6,110.491 ms for DuckGQL
versus 850.439 ms for SQL, or 7.19x. Type/direction fanout statistics now let
the graph optimizer combine indexed endpoints, bounded CSR paths, fixed-hop
CSR expansion, and DuckDB bulk joins without relying only on pattern order.
Complex 13 runs in 55.947 ms through CSR versus 447.275 ms for the normalized
recursive SQL query. Complex 8 improved from 123.553 seconds in the original
run to 12.297 ms, while Complex 9 now completes in 996.762 ms instead of
exceeding 180 seconds. This is an engineering run, not an official LDBC driver
score: it executes reads against the initial SF10 state rather than the
official mixed read/update schedule.

Reproduce against an existing SF10 graph database:

```sh
python3 scripts/benchmark/benchmark_snb_gql_interactive.py \
  --duckdb build/release/duckdb \
  --source-database build/benchmarks/snb10/snb10-relational.duckdb \
  --graph-database build/benchmarks/snb10/snb10-gql.duckdb \
  --relational-results build/benchmarks/snb10/interactive-results-4t-8gb.json \
  --output build/benchmarks/snb10/gql-interactive-results-latest.json \
  --threads 4 \
  --memory-limit 8GB \
  --warmups 1 \
  --runs 3 \
  --query-timeout 180
```

For owned graphs, DuckLake tables can still be exported to graph-header Parquet
and loaded with `COPY GRAPH`. The referenced-table mode above is the zero-copy,
read-only alternative.

## Current limitations

- ISO GQL feature families remain partial or planned; grammar recognition does
  not imply semantic or transactional conformance.
- `CREATE GRAPH`, `DROP GRAPH`, and the full-load `COPY GRAPH` operation are
  autocommit-only. `COPY GRAPH` requires an empty graph.
- CSR construction and CSR algorithms are autocommit-only. Snapshots are shared
  by connections in one database instance, but are not persisted across process
  or database-instance restarts.
- Bulk import currently accepts one vertex file, one edge file, at most one
  scalar vertex label column, and one edge type column. Every relationship row
  must contain exactly one non-empty type.
- Multiple-path and undirected insertion, general runtime property maps, and
  open-graph schema evolution remain incomplete.
- Path modes, searches, shortest-path groups, query composition, procedure
  semantics, named graph types, typed storage enforcement, and the complete
  GQL value/type system remain incomplete.
- Referenced graphs support heterogeneous vertex and edge table mappings,
  mapping-scoped integer source keys, generated snapshot-local element IDs,
  static types, live or pinned snapshots, and read-only access. Composite/string
  identities, multi-table joins for one node record, mapping predicates, and
  write-through mutations are not yet supported.

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
