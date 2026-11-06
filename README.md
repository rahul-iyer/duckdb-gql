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
- Zero-copy typed graph projections over existing DuckDB and DuckLake tables,
  with live or pinned snapshots and snapshot-aware CSR invalidation. Graphs
  registered as `READ_WRITE` accept GQL `INSERT`, `SET`, `REMOVE`, and `DELETE`,
  which write through to the source tables.
- Graph-header CSV, compressed CSV, and Parquet bulk import.
- Directed `MATCH`, `OPTIONAL MATCH`, filtering, projection, aggregation,
  ordering, paging, fixed multi-hop paths, and a bounded variable-length path
  subset.
- Standalone node and directed-path `INSERT`, fixed directed
  `MATCH`-and-`INSERT`, property `SET` and `REMOVE`, and edge/node deletion.
- CSR snapshots that absorb inserts incrementally and are saved in the database
  file, so a reopened database answers graph queries without a rebuild.
- Explicit CSR-backed BFS, DFS, unweighted and weighted SSSP, PageRank, weak and strong
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

DuckGQL currently targets DuckDB `v2.0.0` and uses ANTLR `4.13.2`.

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
platform, so download the artifact matching DuckDB `v2.0.0` and your operating
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

`POWER` follows GQL 2IWD12 clause 20.22 using `DOUBLE` arithmetic.
`POWER(base, exponent)` raises a base to an exponent. NULL stays NULL, and
`POWER(0, 0)` is one. A zero base with a negative exponent or a negative base
with a non-integral exponent raises an invalid-argument error containing
status `2201F`. Non-finite inputs and overflowing results raise status `22003`.
`INT64` exponents retain their exact parity for negative bases.

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

`COPY GRAPH` automatically refreshes DuckDB optimizer statistics and graph
fanout statistics on the imported vertex and edge tables before committing. After subsequent data changes, refresh
these statistics explicitly:

```sql
ANALYZE GRAPH social;
```

The result reports `success`, `graph_name`, and `tables_analyzed`. This command
refreshes native column distinct-count statistics used for join planning, plus
per-edge-type edge counts, distinct source/target counts, and maximum incoming
and outgoing degrees. Collection uses grouped table scans, without building CSR
snapshots or property indexes.

The graph access planner uses average degree among active endpoints to estimate
expansion rows, choose between reachable starting endpoints, and break ties
between eligible expansions. Persisted estimates also work for bounded relational
paths without a prebuilt CSR. A current CSR's statistics take precedence when
available. These are per-type averages, not vertex-label-conditioned fanouts or
multi-hop correlation estimates, and they do not replace DuckDB's SQL join costing.

Graph statistics persist across restarts and are tagged with the graph version.
After a GQL mutation, the planner ignores outdated statistics until the next
`ANALYZE GRAPH`. Direct SQL edits to managed tables require an explicit refresh;
they do not advance the graph version. Explicit transactions do not consume
persisted graph statistics from a separate connection.

Inspect the collected counts and average active-endpoint fanouts with:

```sql
SELECT g.graph_name, s.edge_label, s.edge_count,
       s.edge_count::DOUBLE / s.source_count AS average_out_degree,
       s.edge_count::DOUBLE / s.target_count AS average_in_degree,
       s.max_out_degree, s.max_in_degree,
       s.graph_version = g.graph_version AS current_graph_version
FROM gql_internal.graph_fanout_statistics s
JOIN gql_internal.graphs g USING (graph_id);
```

`ANALYZE GRAPH` supports managed graphs (an empty graph has zero tables). For referenced
graphs, run native `ANALYZE` on the source tables directly. Native SQL `ANALYZE`
and `ANALYZE table_name` retain their usual behavior. Native column statistics follow DuckDB's
lifecycle; checkpoint/reopen can change the estimates, so rerun
`ANALYZE GRAPH` when fresh estimates are needed after reopening.

`DROP GRAPH social` removes both the graph metadata and its managed tables.
`CREATE GRAPH`, `DROP GRAPH`, `COPY GRAPH`, and `ANALYZE GRAPH` are operations that
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
projection. Inserts, through GQL or direct SQL, are added to a small delta that
queries read alongside the snapshot, so the CSR stays usable without a rebuild.
Updates and deletes to a graph's vertex or edge tables invalidate the affected
snapshot instead of allowing an algorithm to use stale data. The next algorithm
call rebuilds its required projection automatically, with concurrent automatic
builders coalesced per graph. Run `gql_build_csr` explicitly only to prepare the
full optimizer/neighbor/inspection snapshot. CSR construction and CSR
algorithms must run in autocommit mode.

`gql_build_csr` also saves the snapshot in the database file, in
`gql_internal.csr_snapshots` and `gql_internal.csr_pages`. After a restart, the
first query loads the saved CSR page by page through DuckDB's buffer manager
instead of rebuilding it, and this also works for databases opened read-only.
Pages are paged in on demand and can be evicted under memory pressure. Updates
and deletes drop the saved copy. Pass `persist := false` to keep a snapshot in
memory only:

```sql
CALL gql_build_csr('social', persist := false);
```

Use `shortest_path_length` for the minimum number of directed hops between two
matched vertices:

```sql
MATCH (source:Person), (target:Person)
WHERE source.name = 'Ada' AND target.name = 'Grace'
CALL algo.shortest_path_length('social', element_id(source), element_id(target))
YIELD distance RETURN distance;
```

The call requires one distinct source and target, returns `0` for the same
vertex and `-1` when the target is unreachable. Optional `vertex_label` and
`edge_label` arguments restrict the paths. Bidirectional search expands from
both endpoints, choosing the side with fewer edges to scan. Its cached graph
projection includes incoming and outgoing edges, so the first call can require
more construction time and memory than an outgoing-only traversal.

Use weighted SSSP to find cheapest directed paths using a numeric edge property:

```sql
CALL algo.weighted_sssp('social', 1, 'cost')
YIELD vertex_id, distance, parent_vertex_id, edge_id
RETURN vertex_id, distance, parent_vertex_id, edge_id;
```

Distances use `DOUBLE` arithmetic. Only reachable vertices are returned, in
settlement order, with distance zero and NULL parent/edge IDs for the source.
`target_vertex_id := 42` stops after settling the target and returns the settled
prefix. `direction := 'in'` or `'both'`, `vertex_label`, and `edge_label` select
the traversal projection. Every selected edge must have a finite, non-negative,
non-NULL weight, including unreachable edges; zero weights and parallel edges
are supported. Distance overflow raises an error. Equal-cost parent choices
are not guaranteed across graph reloads.

Weighted projections are cached separately by property and invalidated on graph
writes. Inspect one with `gql_csr_stats('social', weight_property := 'cost')`;
its `memory_bytes` includes the aligned weight arrays. Weighted SSSP uses a
single worker and an indexed priority queue with O(V) traversal working memory.
Matched calls accept exactly one distinct source:

```sql
MATCH (seed:Person) WHERE seed.name = 'Ada'
CALL algo.weighted_sssp('social', element_id(seed), 'cost')
YIELD vertex_id, distance
RETURN vertex_id, distance;
```

Inspection helpers:

```sql
SELECT * FROM gql_graphs();
CALL gql_neighbors('social', 1, 'out');
SELECT * FROM gql_csr_stats('social');
SELECT * FROM gql_csr_edge_stats('social');
```

`gql_csr_stats` reports the smallest current projection and exposes capability
columns such as `has_outgoing`, `has_incoming`, `has_edge_ids`, and
`has_edge_labels`. `delta_vertex_count` and `delta_edge_count` show inserts not
yet folded into the snapshot, and `loaded_from_disk` shows whether it was read
from the database file. Its `snapshot_acquisition_count` distinguishes consumer
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

### Write to DuckLake tables with GQL

Register a live graph with `ACCESS_MODE 'READ_WRITE'` to let GQL mutations
write to its source tables. The data stays in DuckLake: DuckGQL translates each
mutation into SQL against the mapped tables and columns.

```sql
-- Use the same typed schema and FROM TABLES mapping shown above.
CREATE GRAPH social TYPED { ... }
FROM TABLES ( ... )
OPTIONS (SNAPSHOT_POLICY 'LIVE', ACCESS_MODE 'READ_WRITE');

SESSION SET GRAPH social;

INSERT (:Customer {id: 7, name: 'Edsger'})
       -[:BOUGHT {id: 900, quantity: 2}]->
       (:ProductNode {id: 31, name: 'Notebook', price: 4.5});

MATCH (c:Customer) WHERE c.id = 7 SET c.name = 'Edsger D.';

MATCH (c:Customer), (p:ProductNode)
WHERE c.id = 1 AND p.id = 31
INSERT (c)-[:BOUGHT {id: 901, quantity: 1}]->(p);

MATCH (c:Customer) WHERE c.id = 7 DETACH DELETE c;
```

- A new node goes to the vertex table mapped to its labels, and a new edge to
  the edge table for its type and endpoint node types. Edge endpoint columns are
  filled from the connected nodes' keys.
- The key property is required for new nodes and for edges whose mapping has a
  `KEY`. A key that already exists is rejected, because DuckLake tables have no
  unique constraints.
- `SET` and `REMOVE` update mapped columns by key. Key and endpoint properties
  can't be changed, and neither can labels or edge types, which come from the
  mapping.
- `DELETE` refuses to remove a node that still has edges in any mapped edge
  table, and `DETACH DELETE` removes those edges too.
- Each GQL mutation commits as one DuckLake snapshot, even when it writes
  several tables, and can run inside an explicit transaction.

`READ_WRITE` requires `SNAPSHOT_POLICY 'LIVE'`; a pinned graph is always
read-only. `MERGE` and `INSERT ... RETURN` are not yet supported on referenced
graphs. The same mode works for referenced graphs over an attached DuckDB
database.

DuckLake-backed graphs support multiple vertex and
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
and loaded with `COPY GRAPH`. The referenced-table mode above is the zero-copy
alternative.

## Current limitations

- ISO GQL feature families remain partial or planned; grammar recognition does
  not imply semantic or transactional conformance.
- `CREATE GRAPH`, `DROP GRAPH`, and the full-load `COPY GRAPH` operation are
  autocommit-only. `COPY GRAPH` requires an empty graph.
- CSR construction and CSR algorithms are autocommit-only. Snapshots built by
  `gql_build_csr` are saved in the database file; projections that algorithms
  build automatically, weighted projections, and endpoint statistics are kept in
  memory only.
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
  static types, live or pinned snapshots, and read-only or write-through access.
  Composite/string identities, multi-table joins for one node record, mapping
  predicates, `MERGE`, `INSERT ... RETURN`, and label changes on referenced
  graphs are not yet supported.

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
